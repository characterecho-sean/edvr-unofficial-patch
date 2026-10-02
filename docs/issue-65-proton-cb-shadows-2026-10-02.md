# Issue 65: flat on Linux/Proton spends about 25 ms a frame in `cb shadows`

## Status

*Written 2026-10-02. Update whenever this doc changes.*

- **State:** arturbac confirmed costly reads of DXVK's write-combined mapped
  memory. The adaptive cache is on main (`31f8aee7`). Full build, core rig, 12
  mutants and native Windows flights passed. Proton qualification remains
  pending.
- **Attribution:** [arturbac][arturbac] supplied the diagnosis, staging approach,
  working patch and Proton A/B tests in [issue #65][issue65] and [PR #66][pr66].
- **Report:** issue 65, flat v0.18.0 (build 6ABED11D), GE-Proton 11-6 (DXVK), Mesa 26.2.2
  RADV, RX 7900 XTX, Ryzen 9 9950X, 9000x2160, EDVR -> EDHM -> DXVK through
  `advanced.real_dll`, `temporal_aa = on`. `cb shadows` 25-28 ms per clocked frame
  (about 4500 calls); 49% of the render thread in EDVR's DLL (memcpy 29%, trace ring 14%).
- **What one entry is:** at every Unmap of a pure constant buffer (16 B to 64 KB, up to
  64 tracked) the flat projection runtime copies the WHOLE buffer from the pointer Map
  returned into a 64 KB bank slot (`finishMapFull`, flat_projection_bindings.h:128-137),
  whatever range the game wrote, whether or not a consumer reads that buffer (the
  consumers read about 3 buffers and 10 spans of 16-128 B). The counter spans only
  EDVR's observer bodies, counts scope entries (a Map is 2, an Unmap 1) and clocks 1
  frame in 16. Sean's Windows flat logs (Epic, RTX 5090, native D3D11, 4K): 41-52 ns an
  entry, about 0.11 ms a frame. The reporter: about 5.8 us, roughly 130x.
- **The confirmation** (same spot, SS 0.75, only dxvk.conf differs): without the option,
  17-18 fps for the first ~60 s of passive discovery (EDVR ~52 ms a frame: cb shadows
  ~27, discovery ~23), then 30 fps (cb shadows ~26 ms, ~6 us a call). With
  `d3d11.cachedDynamicResources = c`: 60 fps with VSync, about 78 without (present p50
  12.9 ms, EDVR ~6.5 ms), cb shadows ~3.3 ms at ~4050 calls (~0.8 us a call). Their PDB
  build with AMD LBR call records: 94% of the render thread's memcpy calls come from
  `observeUnmap` -> `finishMapFull` (flat_projection_runtime.cpp:344); during discovery
  `flatTemporalUnmap` and `flatTemporalUpdate` (flat_temporal.cpp) make the same copy,
  plus `projectionHashes`. Their first test of the option was void: DXVK never loaded
  it (the Steam Linux Runtime container did not mount the config's directory).
- **Workaround (Linux/Proton):** a `dxvk.conf` next to `EliteDangerous64.exe` holding
  `d3d11.cachedDynamicResources = c`. DXVK's log then lists it under `Effective
  configuration:`.
- **Still open:** with the option, 0.8 us a call is still about 17x Windows (a slow
  QueryPerformanceCounter under Wine is the candidate; their `flat cpu 5s` lines show
  the clock floor). The trace ring (always recorded, read only by an F10 dump, a
  504-byte event built twice per mark) is 14% of the render thread without the option
  and about 5% with it, while its own counter shows about 0.3 ms. Native Windows on AMD
  is unchecked: Sean's NVIDIA logs show no such cost, but other drivers may also hand
  out write-combined memory.
- **Ruled out:**
  - ruled out: lock contention, because the path takes no lock (an owner-thread check
    only);
  - ruled out: the bank and tracked-list scans as the difference, because the same code
    costs 10-15 ns of work per entry on Windows;
  - ruled out: DXVK's own time as the cost, because DXVK is about 1% of their render
    thread.
- **Fix:** the map bounce (below). Where reads of the mapped pointer are slow, hand the
  game a cached slot at `Map(WRITE_DISCARD)` of a tracked constant buffer and write it
  into the real mapping at Unmap, so every EDVR reader, discovery's included, reads
  cached memory. Native NVIDIA keeps the v0.18.0 path. Later: the trace ring, the
  `track()` negative cache, a demand-driven shadow.
- **Temporary validation key:** `advanced.flat_cb_map_cache = auto|on|off`; default
  `auto` measures mapped reads, `on` forces caching and enables verification,
  `off` keeps direct mappings. Removal requires Sean's approval when the arc closes.
- **Next flight:** Proton qualification of main's cache, comparing `auto`, `on`
  and `off` after restarting; collect decisions, counters and frame timings.

## Contribution and attribution (2026-10-02)

[arturbac][arturbac] identified the expensive constant-buffer shadow copies in
[issue #65][issue65]. Their PDB build, perf profiling and AMD LBR call records
traced the cost to CPU reads of DXVK's write-combined mappings. Their
[confirmation and staging proposal][diagnosis65] established the workaround and
proposed returning cached memory at `Map(WRITE_DISCARD)`, then writing it back
at `Unmap`.

They supplied the working staging implementation and Proton A/B validation in
[PR #66][pr66]: about 26 ms to 0.35 ms per frame for CB shadows and about 30
fps to 80-90 fps in the reported scene. The adaptive map-cache implementation
on main builds on that diagnosis and staging approach, adding adaptive
selection, lifetime guards and validation. Credit for the original diagnosis,
approach, PR implementation and Proton measurements belongs to arturbac.

The original implementation and merge commit messages omitted this attribution.
This entry, the README and the release-note acknowledgment correct that
omission.

[arturbac]: https://github.com/arturbac
[issue65]: https://github.com/characterecho-sean/edvr-unofficial-patch/issues/65
[diagnosis65]: https://github.com/characterecho-sean/edvr-unofficial-patch/issues/65#issuecomment-5951666303
[pr66]: https://github.com/characterecho-sean/edvr-unofficial-patch/pull/66

## RVA map (the v0.18.0 release DLL)

The release DLL carries no CodeView entry (build.bat links a PDB only with
`EDVR_PROFILE_SYMBOLS=1`), so no PDB can match it. The RVAs were resolved by
disassembly, `.pdata` and the source. To get names from a PDB, build the v0.18.0 tag
with `EDVR_PROFILE_SYMBOLS=1` and compare the `.text` sha256 with the release's
(`e952a174687245f2fdecca004c8444c119890cd431ebd4f76405562b7a2c4457`, vsize 0x2deac0).
The reporter's own PDB build agreed on the top two.

| RVA | Share | Function | Confidence |
|---|---|---|---|
| 0x2d3f90-0x2d4012 | ~29% | CRT memcpy (0x2d3dc0-0x2d442d), the 256-byte AVX loop for copies over 256 bytes | exact |
| 0x0b9900-0x0b9a11 | in ~14% | `flatTraceEventMarker` (flat_trace.h:81): zeroes one 0x1F8-byte event | exact |
| 0x0b9a20-0x0b9be4 | in ~14% | `flatTraceMark` (flat_trace.h:159): two 0x1F8 temporaries in a 0x420 frame, capped at 4096 events a frame | exact |
| 0x09e720-0x09ea0a | 1.8% | `capture(Camera&, mapped)` (flat_runtime.cpp:1238): 96 B of camera rows read from the mapped pointer | strong |
| 0x07f600 | 1.2% | `FlatProjectionRuntime::observeUnmap` (flat_projection_runtime.cpp:340), its shadow memcpy at 0x07f72a | strong |
| 0x0c6f00 | 0.9% | the "flat stand-down: still stood down" log formatting, so their window held stood-down frames | exact |

Environment note: DXVK's built-in profile for Elite sets `dxgi.customVendorId = 10de`,
so EDVR sees the reporter's AMD card as NVIDIA.

## Fix design: the map bounce (2026-10-02, implemented; flights pending)

Designed read-only against origin/main d7131071; every file cited is unchanged since
v0.18.0, so its line numbers hold. Nothing built or flown. For the implementer: follow
AGENTS.md; branch `claude/issue-65-map-bounce` from origin/main; several sessions share
this machine, so run `python tools\build_lock.py --wait` before each `build.bat`. The
cause, confirmed: DXVK gives dynamic buffers uncached BAR memory (d3d11_buffer.cpp
GetMemoryFlags: DYNAMIC -> HOST_VISIBLE|COHERENT|DEVICE_LOCAL), and EDVR reads the whole
mapped buffer at every Unmap. The name "map bounce" avoids flat_substitution.h, which
already means engine motion's substituted draws.

### 0. Decisions

1. Hook: in hookedMap, right after the real Map succeeds and BEFORE every consumer of the
   pointer (vscreen.cpp:3280-3292, 3369), swap `mapped->pData` for a cached slot; in
   hookedUnmap, at the TOP, before the early returns (3401, 3407), copy slot -> real
   pointer. Every reader in section 2 then reads cached memory unchanged.
2. Applies to WRITE_DISCARD maps of tracked pure DYNAMIC constant buffers, owner context
   and thread, flat runtime live, frame not Paused. Everything else passes through
   (v0.18.0 behaviour).
3. Storage: a static arena, 8 slots x (64 KB + 64 B guard), 64-byte aligned. Not the
   bank: the bank dies with the runtime (stand-down, resize) while the game holds the
   pointer, and Entry::bytes sits at +0x1E, unaligned (flat_projection_bindings.h:55).
4. Bytes at Unmap: the whole tracked ByteWidth (a DISCARD gives the GPU a fresh slice,
   so the tail must be defined). Seed the slot from the valid shadow, else zero.
5. Adaptive: time the existing Unmap copies and bounce only where reads are slow, so
   native NVIDIA behaves exactly as v0.18.0. A TEMPORARY key
   `advanced.flat_cb_map_cache = auto|on|off` serves validation flights
   (runtime_profile.h:105-114 allow-list, edvr.ini entry); list it in this doc's Status
   block and remove it at close, per AGENTS.
6. Fail-safe trips (stop bouncing, flush what is open, log why): map while open, open
   across a Present, context mismatch, verify mismatch.
7. In this fix: the bounce, the decision, counters and the log reader. Later: section
   4, including the `track()` negative cache (an optimisation, not the cause).

### 1. How Elite maps the buffers we shadow

Measured (Sean's Epic flat logs, Windows NVIDIA, native D3D11):

- Widths: F10 draw captures (12 frames, 3840x2160, first 512 of ~12.7k draws each, 4,480
  draws; flat_draw_pixels\*\frame_*.json cb[] = VS slots 0-2, flat_draw_capture.h:138).
  VS b1 is the scene CB, 5,376 B (336 float4, flat_camera_producer_probe.cpp:52), one
  buffer per session; VS b0 208 B; VS b2 per-object CBs 48-1,008 B, 290 distinct buffers
  over the 12 frames.
- Offsets: first_constant 0 and constant_count 4096 on every recorded draw and slot. A
  reused CB bound at offset 0 cannot take NO_OVERWRITE appends, so the scene CB must be
  DISCARD or UpdateSubresource (inference). Unmeasured: slots 3+, draws past the cap.
- Distinct CBs written per frame: 1 large (>= 3,776 B), 6-88 small (flat discover
  projection-summary cb-pool-used, logs 20261001_142856 and _151627).
- Writes per frame: trace dump edvr_logs\traces\flat_trace_47675.bin (4K, 3 frames):
  1,204-1,356 draws, 196-531 write events over 82-93 resources, 33 camera captures
  matched by one resource written 33 times (the scene CB). Hangar: ~100 camera writes
  (flat_camera_table.h:13-14). Tracked-CB full writes ~210-250 (rc.3-8, 450 frames per
  5 s line). Maps ~1,100 (vscreen.cpp:3258), 3,110 over terrain
  (docs\engine-render-performance-2026-09-19.md:2515). The scene CB alone: 33-100 x
  5,376 = 177-540 KB a frame.
- No deferred contexts, no foreign threads: unknown-lists=0 and foreign-thread-calls=0 in
  all 762 discovery windows of 44 Windows flat logs (~1.1M presents, 134M draws).

Unknown, measured in flight 1 at no extra D3D cost:

- (a) The Map type split, Usage and CPUAccess of tracked CBs: track() already calls
  GetDesc (flat_projection_runtime.cpp:279-282); count tracked maps by type in
  observeMap (:327) and log the first 64 registrations once.
- (b) An Unmap width histogram (<=64, 256, 1K, 4K, 8K, 64K) and bytes a frame.
- (c) Whether the game rewrites whole CBs: at each flush, count the 16-byte rows still
  equal to the seed (a lower bound on untouched rows, since a row rewritten with the same
  bytes also counts). Nothing in any mode changes what reaches the GPU for this.
- (d) Offsets at slots 3+: count nonzero cbFirst in flat_context_state.h captureStage
  (:296-330), fetched whenever engine motion saves state.

### 2. Every flat-path read of a game-mapped pointer

| # | Reader | Where (origin/main) | Reads | Swap covers it |
|---|---|---|---|---|
| 1 | bank shadow | observeUnmap flat_projection_runtime.cpp:340-349 -> finishMapFull flat_projection_bindings.h:128-137 (memcpy :134); mapBytes :338 | whole width, each tracked CB, ~210-250/frame | yes |
| 2 | camera rows | flatRuntimeUnmap flat_runtime.cpp:2442-2445 -> capture :1238 -> flat_camera_table.h:124-130 | 96 B at +4,320, 33-100/frame | yes, if setMapped (:2435) gets the new pointer |
| 3 | discovery | flatTemporalUnmap flat_temporal.cpp:1200-1216: memcpy min(width,4096) :1207, 96 B :1209, 16 B at +4,496 and +5,312 :1210-1211; projectionHashes (:486) hashes the copy | each CB <= 64 KB, first 120 s / 12,000 frames (kCaptureMs :52) and per F10 | yes: nearly every CB is tracked at Map time; the rest count as `untracked` |
| 4 | engine motion tee | noteResourceWrite engine_velocity.cpp:2296-2323 (rows :2307-2310); pointer from vscreen.cpp:3369 | 96 B, watched scene CB | yes |
| 5 | census CB watch | drawCensusCbNoteUnmap vscreen.cpp:3425 | <= 768 B, F10 only | yes |
| 6 | cold readback | pollColdReadbacks .cpp:229-261 | EDVR's own STAGING copy (DXVK HOST_CACHED) | n/a, fast |
| 7 | EDVR's own buffers | flat_projection_scope.cpp:79 (a write), flat_compute_readback.cpp, flat_mono_resolve.cpp:485, flat_draw_capture.h | g_flatComputeInternal | n/a |

flatTemporalUpdate (:1217) and observeUpdate read the game's own pSrcData: cached, fine.

VR side, out of scope, named: the composite CB memcpy (vscreen.cpp:3431-3439), the camera
block readers (glitchFrameObserve, sunglareSceneDump/Rows, temporalPassNoteSceneWrite
:3440-3467), the scene CB :3468, particleCapture :3476, billboardCapture :3483,
glitchFrameObservePool :3418, fssRevealNoteUnmap :3428, the engine motion copier
(engine_velocity_primary_copy.h:132-133: 336 B reads of the mapped POOL, 0 calls in 8
Windows flat logs). VR on Proton has the same problem class.

### 3. The design

Flow. "Tracked" = a pure CB (BindFlags == CONSTANT_BUFFER, 16 B-64 KB, multiple of 16)
in the projection runtime's 64-slot table, registered by track()
(flat_projection_runtime.cpp:275-311), which evicts an unpromoted entry to fit a new CB.
hookedMap: real Map; call track(); if eligible, claim a slot, seed it, set
`mapped->pData` = slot; then the existing tees run on the slot. Claim and seed BEFORE
resourceWritten, which invalidates the shadow (flat_runtime.cpp:2425). hookedUnmap: an
RAII guard takes the record for `res`, copies slot -> real pointer, the existing tees
read the slot, the guard frees it on every return path, then the real Unmap. Width and a
held reference (ComPtr, flat_projection_runtime.h:108) come from the tracked entry. The
flush needs neither the runtime nor the owner thread: records (res, ctx, real, slot,
width, frame) sit in a static table of 8 with atomic keys; Map claims on the owner
thread only, Unmap may come from any thread. Header-only (flat_map_bounce.h): no new
translation unit for the reporter's build.

Decision. In observeUnmap, time the first 32 copies of >= 256 B (QPC, 4 batches of 8; a
batch of 0 ticks is fast). Slow if the median batch rate is under 1 byte/ns (cached >= 5;
uncached/BAR 0.02-0.5). Frozen for the session. While pending, and when off, every map
passes through.

| Case | Decision | Why |
|---|---|---|
| NO_OVERWRITE, WRITE, READ* | pass through, count | a kept pointer would lose writes; offsets are 0, so rings are unlikely; needs a coherent mirror |
| Map fails, pData null, sub != 0, WAS_STILL_DRAWING | no record | nothing mapped |
| deferred or other context (`self != ownerCtx`, vscreen.cpp:1459) | pass through; flush checks record.ctx, mismatch = no flush + trip | hook returns at :3248 |
| non-owner thread, same context | no bounce at Map (owner() test, flat_runtime.cpp:2433); Unmap flushes from any thread | flush is a memcpy |
| g_flatComputeInternal (EDVR's own maps) | pass through | returns at :3240 |
| Paused, stood down, no projection, jitter off | no bounce; flush ignores this state | no reader exists |
| Map without Unmap | ANY Map of an address with an open record drops it unflushed (declined maps too: stale address reuse); 3 drops = trip; at Present, older records are logged and kept for a late Unmap | a stale `real` must never be written |
| width > 65,536, 8 open, no desc | decline | arena size |
| alignment | 64 B plus a 64 B zero guard | games use aligned SIMD stores |
| EDHM/3Dmigoto | EDVR is outermost (EDVR -> EDHM -> DXVK): the game sees the slot, EDHM and DXVK see the real pointer; the flush precedes realUnmap, so they see final bytes | order of the chain |

Cost. DXVK, decision on: ~50 ns per eligible Map and one sequential write of the width to
BAR/WC per Unmap (5.4 KB ~0.3-1 us): ~0.1-0.2 ms a frame against 20+ ms of reads removed.
Native NVIDIA, decision off: an atomic load and a few counter adds per Map (~10 us a
frame) plus 32 timed copies once.

### 4. Further cuts

- `track()` negative cache: LATER. Every Map/Update of an untracked resource pays
  QueryInterface + GetDesc + Release (flat_projection_runtime.cpp:278-282), ~1,000 a
  frame. That is not the issue 65 cause (DXVK is about 1% of the reporter's render
  thread), so it waits for a measurement. Its shape when it comes: a memo keyed by
  pointer and a frame generation (bumped in pollColdReadbacks, once a Present) so it
  cannot go stale, holding no references.
- Trace ring (always recorded, a 0x1F8-byte event built twice per mark, flat_trace.h:81-86,
  150-164): LATER, its own commit. Counter ~0.3 ms, perf 5% (14% without the option):
  re-measure on the fixed build. Safe shape: 16-byte marker records beside the draw ring,
  merged at dump by a draws-before count, so EDVRFTR4 and the replay rig are unchanged.
  "Armed only" changes the F10 procedure; not now.
- Demand-driven shadow (~3 buffers, ~10 spans of 16-128 B are consumed): LATER; it
  changes plan readiness and needs the cold path, and the bounce already makes the copy
  cheap.
- NO_OVERWRITE mirror: only if flight 1 shows NO_OVERWRITE on tracked CBs.
- Discovery's 120 s default window (~23 ms a frame on Proton, 5.5 on busy Windows
  frames): Sean's call (F10-only or shorter), not this fix.

### 5. Instruments

- Armed line, once: `flat map bounce: decision at frame F, batch rates a/b/c/d B/ns,
  threshold 1.0 -> ON|OFF, key=auto|on|off`; at 30 s with no decision: `pending n/32
  qualifying copies`.
- `flat map bounce 5s:` every window while a temporal mode runs, zeros included (an
  absent line means the block never ran; the flat_runtime.cpp:2146-2175 pattern): state,
  key; maps by type (tracked CB / other buffer / texture); bounced; declined by reason
  (not-discard, foreign-context, foreign-thread, internal, paused, untracked, open-full,
  width); flushes, bytes written at Unmap, flush ns/KB; bank read bytes and ns/KB;
  abandoned, open-at-Present, trips; verify samples and mismatches (key=on only, memcmp 1
  flush in 16: slow on Proton, so validation only); width buckets; unchanged rows
  (section 1 (c)).
- Time install and flush inside the flatcpu kShadows scopes (no new family:
  flat_cpu_tests.h and the log parsers stay valid). Add `--map-bounce` (PASS/WARN/STOP,
  self-test fixture) to tools\edvr_log.py.

| Run | cb shadows per entry | discovery | new line |
|---|---|---|---|
| Windows auto | 41-52 ns as v0.18.0 | as v0.18.0 | state=off, bounced 0, bank ~30-60 ns/KB |
| Windows key=on | +50-150 ns | as v0.18.0 | bounced ~100%, 180-540 KB/frame, verify mismatches 0, trips 0 |
| Proton, no dxvk.conf, working | ~6 us -> under 1 us (their `c` run: 0.8) | ~23 ms -> ~5 ms | on; bank ~30-60 ns/KB (was ~30,000); flush ~0.1-0.3 ms/frame |
| Proton + `c` | ~0.8 us | as above | off (reads fast) |
| never ran | ~6 us | ~23 ms | absent = hooks not reached; pending forever = no copy >= 256 B; off with ns/KB in the thousands = wrong decision |

### 6. Tests and flights

Rig `tools\flat_map_bounce_test` in house style (panel_curve_test + mutants.py;
flat_camera_table's Faults template): `:rig_flat_map_bounce_test` in build.bat beside
`:rig_panel_curve_test` (:2948) with --dry-run, --self-test, mutants.py --self-test. No
D3D device: the core takes a driver interface.

- Fake driver: DISCARD returns a fresh slice (poison 0xDD, guards 0xEE) per Map;
  NO_OVERWRITE/WRITE a persistent one; injected failures (E_FAIL, WAS_STILL_DRAWING +
  null pData); Unmap commits to a "GPU view".
- Game model: seeded random programs over 6 resources, 2 contexts, 3 thread ids:
  Map(type), partial and full writes through the pointer (ptr % 64 == 0 when bounced),
  Unmap on the same or another thread, abandoned maps, re-Map while open, Present,
  decision and Paused/reset flips between Map and Unmap.
- Oracle: GPU view == the bytes written on the last committed map (rest undefined, guards
  intact); shadow == the same; declined maps get exactly the real pointer; flush bytes ==
  width. Checks C1-C8: DISCARD round trip, passthrough, failures, interleaving,
  thread/context mismatch, abandon and Present trips, alignment and guards, the decision
  around the threshold (frozen after).
- Mutants, each must fail a labelled check: skip flush; width -1/+1; flush after the real
  Unmap; bounce NO_OVERWRITE; accept a failed Map; no drop on re-Map; skip flush off the
  owner thread; arena +8; reuse an open slot; inverted threshold; verify off.
- flat_projection_runtime_tests.h:190-197 (a real WARP Map(WRITE_DISCARD)): add
  observeMap/observeUnmap fed a bounce pointer, shadow == written.

Flights: (1) Sean, Epic, auto: decision OFF, counters as v0.18.0, fills section 1's
unknowns. (2) Sean, Epic, key=on, 15 min or more over a station, an SRV, on foot,
hyperspace, F10, F8 mode changes and a resize: verify mismatches 0, trips 0, treated
streaks and refusals as before. (3) Optional: DXVK on Windows (docs\dxvk-windows.md),
expect ON. (4) The reporter: branch `claude/issue-65-map-bounce`, built as their 0.18.0
was (no third_party added): no dxvk.conf (expect ON, 60+ fps), with `c` (expect OFF),
key on/off. Their local copy-at-Unmap patch is the same idea; ask for the diff.

### 7. Open risks

- Elite may rewrite DISCARD'd CBs partially and rely on old tails: seeding approximates
  it, and the unchanged-row count estimates it.
- Map types, Usage and slots 3+ stay unmeasured until flight 1; NO_OVERWRITE on a tracked
  CB keeps the slow read on Proton (status quo, counted).
- Windows is protected by the decision alone: exotic memory (Windows + AMD write-combined,
  1-3 GB/s) reads "fast" and keeps v0.18.0 behaviour; a false "slow" costs ~0.3 us per
  Unmap.
- An Unmap the hooks never see would lose writes: the Present watchdog and the trips
  detect it, and key off is the way back. No fence added: the game relied on the same API
  contract for its own stores.

### 8. Implementation and local validation (2026-10-02)

Implemented the header-only cached mapping core, projection seed/copy callbacks,
Map/Unmap hooks, independent Present epoch, temporary flat-profile key, and the
`--map-bounce` log verdict. The arena and its held resource/context references
survive runtime resets. An unsafe event permanently stops new bounces; a valid
open mapping remains available for its matching late Unmap. A repeated Map drops
the old record without writing a potentially stale driver pointer.

Native auto-OFF skips slot scans when none are open and measures only the first
32 qualifying bank copies. The unchanged-row metric requires an additional
512 KB cached baseline arena; copies touch only the tracked width. Registration
logs are capped at 64, and the census includes map types, width buckets, sampled
versus total bank bytes, nonzero CB offsets, and explicit trip reasons.

Local checks: core C1-C8, 1,200 seeded six-resource programs, actual foreign-thread
Unmap, and all 12 fault mutants passed their checks. The log reader self-test and
configuration contract pass. Independent review found no writeback or lifetime
blocker; its telemetry population finding was corrected before building. Full
production/WARP/installer validation passed and wrote the full-build receipt.
The source-pin test now expects the two additional shadow scopes. The runner's
process-tree timeout check requires an unsandboxed build; its sandboxed failure
was reproduced and the unchanged check passed outside the sandbox. Current
main's F10 shader capture was included in the validated source. Epic testing uses
the flat profile, existing DLSS settings, and the default `auto` key. Promote the
clean DLLs with the matching receipt and preserve the live INI when installing.
Subsequent native Windows flights passed in auto-OFF and forced-ON modes.
Proton measurement of this implementation remains pending.
