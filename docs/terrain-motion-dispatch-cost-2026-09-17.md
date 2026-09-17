# DLSS doubles the frame over terrain (2026-09-17)

## Status

- State: the per-patch dispatch was NOT the carrier. The batched build
  (a17c793, on main) was flown 14:04 and 14:12 on Frontier at 65%: correct
  (`Batched build: 1742 batches, 12.3 us/eye`, all 85,024 patches original
  draws, 0 reissues) and the frame did not move (benchmark over terrain
  gpu p50 11.1 / cpu 5.1 ms, app render 9.9-10.7, door 3.7-3.9 per pair:
  the same ~2.2 ms outside every bracket). Sean: "performance does seem
  to be somewhat improved" -- most likely the 65% vs 75% HMD quality of
  the 12:29 flight. Two offline GPU micro-benchmarks on the RTX 5090
  (scratch, journal below) price the whole hook: about 0.65 ms a frame at
  97 patches, nearly all of it the three `CopySubresourceRegion`s (a fixed
  ~2 us each between real draws; the PS + 2 MRTs on the prepass cost 0.02-
  0.15 ms per 100 patches, the render-thread D3D11 calls 0.3 us a patch).
  The old dispatch cost about the same, which is why the flight read flat.
- ATTRIBUTED (flight 150849, instrument build 185ceee, parked on the
  planet, eye dumps with the draw census under dlss AND off): the gap is
  not one carrier but the SUM of EDVR's per-draw GPU syncs inside the
  game's passes. The census diff (frames 1-2 of each, per frame, dlss
  minus off): +306 `CopySubresourceRegion` + 102 `UpdateSubresource(48 B)`
  (the terrain hook, 51 patches per eye), +37 `Dispatch(1,1,1)` of one
  compute shader (the cockpit holo hook, per draw) + 10 of another (the
  stellar ring hook: `stellar coverage GPU ... 0.189 ms/frame`), ~50
  mesh/object-probe copies, +38 query `End`s (EDVR's timestamps); draw
  counts equal. Priced offline on the 5090 with real fill between draws:
  the terrain hook 0.7-0.78 ms per 100 patches, a `Dispatch(1,1,1)` with
  CS churn 4.75 us, a timestamp pair 0.00 (free), a copy ~2 us. Sum
  1.25-1.8 ms against a gap of 1.3-1.7 (benchmark gpu p50 dlss 10.2-10.6,
  off 5.1, door 3.65 per pair); in situ the syncs run ~1.5-2x the
  synthetic price (the mid-frame bracket read ~14 us per patch against 7).
- ruled out (same flight): the game's `Map` stalling on the GPU --
  `Game Map ... reads 4 calls 0.003 ms/frame, writes ~1100 calls 0.10-0.17
  ms/frame` under dlss, 0.09-0.12 under off; the terrain hook's own CPU --
  `Hook CPU: 0.62-0.68 us/call, 0.024-0.029 ms/frame`; EDVR's timestamp
  brackets -- free in the benchmark. The benchmark's CPU rise (still
  +1.6 ms at dlss over terrain) remains unexplained by any EDVR call the
  probes time; it is not the terrain hook, not Map, not the door.
- The optimisation pass, in order of size, all "remove the per-draw GPU
  sync": (a) BUILT on this branch (journal, latest entry), NOT FLOWN: the
  terrain hook captures the three constant segments on the CPU at the
  game's own write (tees in vscreen.cpp on Map/Unmap and
  UpdateSubresource; CopyResource/CopySubresourceRegion destinations,
  executed command lists and CreateBuffer invalidate), memcpy per draw,
  one UpdateSubresource per eye at the batched build; the GPU copy stays
  as the fallback for a slot whose shadow is not current. Census tail
  `Constants: N slots from the CPU shadow, M by GPU copy, R re-watches;
  tee copies X us each over W writes, Y ms/frame`. The rig runs its suite
  three times (UpdateSubresource tees, Map/Unmap tees, no tees). Expected:
  -0.6..-0.9 ms/frame over terrain. (b) the holo hook's per-draw dispatch
  batched like mesh/terrain (-0.2..-0.4). (c) the stellar ring hook's
  per-draw dispatch batched (-0.19). What remains after those is NVIDIA's
  own 2.9-3.1 ms per pair plus prep/ui 0.7, the foveated-DLSS arc's lever.
- Instruments in the build: `native timing CPU ... Game Map over N frames:
  ...`; `terrain motion GPU ... Hook CPU: ... Constants: ...`; live levers
  `advanced.terrain_motion` / `advanced.mesh_motion` (on/off, default on,
  ini edit; not on the menu).
- Next flight: (a) installed on Frontier; the same parked A/B (dlss vs
  off, an eye dump with the census in each state) plus, if an ini edit is
  possible mid-flight, `advanced.terrain_motion = off` under dlss for the
  residual. Read: benchmark gpu p50 under dlss down ~0.7 ms; `Constants:`
  nearly all from the CPU shadow with few re-watches; `tee copies` well
  under 1 us each (mapped memory read as cached) -- if it reads several
  us each the mapped memory is write-combined and the next lever is to
  hand the game a cached scratch pointer at Map and stream it to the
  driver's at Unmap; the census diff showing the 306 copies gone.
- Report: Sean, Frontier install, Pimax Crystal Super, after the terrain
  history fix (docs/terrain-history-shimmer-2026-09-17.md): "performance is
  now quite bad flying low to the surface" (log 122915, HMD quality 75%,
  3054x2545 in), then his A/B in log 124139 at 65% (2646x2206 in, 4072x3394
  out): "DLSS effectively doubles the gpu and cpu frametime numbers".
- The A/B in the log (native benchmark windows w6-w11, gameplay, live
  toggles of fix.temporal_aa): off cpu 2.8 / gpu 4.5-4.9 ms; dlss cpu
  4.7-5.2 / gpu 10.9-11.0; fsr 4.4 / 9.3. `Application-render GPU` over
  terrain: off 4.4-4.6 ms, dlss 10.2-10.7. The door brackets under dlss
  (`temporal aa price`) hold prep 0.46-0.49 + full 2.82-2.97 + ui 0.34 =
  3.6-3.8 ms per pair, so 2.2-2.4 ms a frame of EDVR's GPU sits OUTSIDE
  every bracket. In space (12:43:04-24, before the terrain shader compiled)
  the same brackets and an app render of 4.7-5.2 ms leave at most ~1.5 ms
  outside, the game's own render included: the cost arrives with the
  terrain.
- First hypothesis, REFUTED (journal, later entry): the per-patch
  `Dispatch(1,1,1)` in celestial_motion.cpp `begin()`. a17c793 replaced it
  with three 416-byte constant snapshots per draw and one `Dispatch(count)`
  per eye at `celestialMotionViews(ctx, ...)` or the frame boundary; the
  records, keys, matching and the rig are unchanged and it stays (it is
  correct and no dearer). The `ruled out:` lines are in the journal.
- Ruled out along the way: the texture LOD bias as the live A/B's delta;
  the PS + 2 MRTs over the prepass beyond ~0.15 ms; the hook's D3D11 calls
  as the CPU rise (0.3 us a patch through the runtime); the game blocking
  in GetData. Still unpriced in flight: EDVR's own wrapper cost per hooked
  call (`Hook CPU`), the holo hook's per-draw dispatches (in space too, so
  bounded by orbit's ~0), the depth/luma probes (a constant in every flight
  compared), HMD quality 75% vs 65% (Sean's slider), outer trim 5 vs 10.

## Journal

### 2026-09-17 (night) -- (a) built: the terrain constants captured on the CPU

main (af203b1, the weapon-stability retirement) was fast-forwarded into
the branch first; this is the last workstream before the release.

What changed (celestial_motion.cpp, vscreen.cpp, device_hook.cpp, the
rig): the three per-patch `CopySubresourceRegion` of the game's VS b0/b1/
b2 into the build's input rows are gone from the common path. The hook
watches the three buffers bound at the terrain draw by pointer (a
re-watch whenever a slot's pointer changes; the first draw after one
takes the GPU copy) and vscreen.cpp's hooks tee the game's own writes to
them: `hookedMap` remembers the mapped pointer, `hookedUnmap` copies the
slot's segment out of it BEFORE the real Unmap, `hookedUpdateSubresource`
copies the overlap of the write's box with the segment (a write covering
the whole segment validates the slot, a partial one onto an invalid slot
leaves it invalid), `CopyResource` / `CopySubresourceRegion` destinations
and an unknown `ExecuteCommandList` invalidate, and the device's
`CreateBuffer` invalidates a matching address (a buffer destroyed and
re-created at the same pointer with initial data would otherwise inherit
the dead one's shadow). `begin()` memcpys each valid slot's segment into
a CPU row and `flush()` uploads the new rows with one boxed
`UpdateSubresource` per eye before the batched build (per segment when a
row mixes paths, never over a GPU-copied segment).

Only the segments are shadowed (128 + 64 + 224 bytes), never the whole
buffer: the game writes these buffers through Map ~1100 times a frame
and a mapped dynamic buffer is most likely write-combined memory, whose
reads are uncached (~100 ns a line), so b1's 5376 bytes would have cost
more than the 64 the build reads. Whether the reads are cheap is not
known until flown, so the copy is timed: `tee copies X us each over W
writes, Y ms/frame` on the census tail, alongside `Constants: N slots
from the CPU shadow, M by GPU copy, R re-watches`. Stale DLL: no
`Constants:` on the `terrain motion GPU` line. Tees never wired: every
slot `by GPU copy`, none from the shadow. Working: nearly all slots from
the shadow, re-watches in single digits, `tee copies` under 1 us.

The rig (`celestial_motion_test`) runs its whole suite three times --
tees around UpdateSubresource, tees around a real Map/Unmap on DYNAMIC
buffers, no tees -- and adds: an unknown write to one slot sends exactly
that slot through the GPU copy while the other two stay on the shadow; a
boxed sub-range write onto a valid shadow updates just those bytes and
keeps all three on the shadow; a full-segment write after an unknown
write revalidates; a partial one does not (that slot falls back, the
record still reads right); after a Map/Unmap the next draw takes all
three from the shadow.

Not this entry's evidence, only its expectation: -0.6..-0.9 ms a frame
over terrain, the 306 copies gone from the census diff. The holo (b) and
stellar-ring (c) dispatches are next, same pattern.

### 2026-09-17 (evening) -- flight 150849 on 185ceee: the census diff attributes the gap

Sean, Frontier, parked on the planet, an eye dump under dlss (15:10:43,
census 1, frames 11475-11477) and one under off (15:11:03, census 2),
`fix.temporal_aa` flipped on the menu between them. Benchmark: w10-w12
dlss cpu 4.6-4.8 / gpu 10.2-10.6; w13 off 3.1 / 5.1. App render dlss
10.0-10.4 (10.98 with the dump), off 4.35-5.36; price under dlss prep
0.33-0.46 + full 2.62-3.13 + ui 0.36.

The probes: `Game Map over 448 frames: reads 1864 calls 0.003 ms/frame,
writes 464672 calls 0.142 ms/frame, longest 1.133 ms` (dlss, parked);
`reads 1688 calls 0.002, writes 488929 calls 0.115` (off). `Hook CPU:
0.62 us/call over 532470 calls, 0.029 ms/frame`. Both hypotheses of the
morning entry are dead; the CPU rise stays unexplained (it is not in any
call the probes time -- a candidate for a per-hook-family CPU accumulator
if it ever matters on its own; the GPU is the bound).

The census diff (scratch `census_diff.py`, frames 1 and 2 of each census,
per frame, dlss minus off; frame 0 carries the dump's own copies):

| per frame | dlss | off | what |
|---|---|---|---|
| copies | 2060 | 1568 | +306 terrain (3 x 102 patches), +50 mesh/probe (32 KB -> 1 MB pool, 96-stride pool), the rest unresolved ids |
| UpdateSubresource | +102 into a 48 B buffer (terrain b13), +42 into the holo draw buffer | | |
| dispatches | 109 | 59 | +37 `58CB11289E1263B3` (1,1,1) holo per draw, +10 `ED270307A4591BAA` (1,1,1) stellar ring, +2 each of the door's and the batched builds (n=51 terrain, n=19 mesh) |
| query Begin/End | 142 | 82 | EDVR's timestamp brackets |
| eye draws | 570 | 564 | equal |
| offscreen draws | 640 | 650 | the same 2974x2602 HUD surfaces; off alone has a 256x256 mip chain (42 copies + 9 draws), which cuts the other way |

Micro-benchmark 2 extended (256x256 grids, 100 patches, GPU ms per
batch): depth-only 0.542; + a timestamp pair around EVERY draw 0.544; +
a pair every 64th 0.544; + `Dispatch(1,1,1)` with CS churn per draw 1.017
(4.75 us each); the a17c793 hook 1.239-1.322. So per frame over terrain:
terrain hook 0.75 (x1.5-2 in situ), 47 dispatches 0.22-0.45, stellar
0.19 (its own bracket), mesh/probe copies ~0.1: 1.25-1.8 ms, the gap.

- ruled out: the game's Map waiting on the GPU, because the probe reads
  0.003 ms/frame of READ maps and 0.10-0.17 ms of WRITE maps under dlss,
  0.09-0.12 under off.
- ruled out: the terrain hook's render-thread CPU (EDVR's wrappers
  included), because `Hook CPU` reads 0.62-0.68 us a call, 0.03 ms/frame.
- ruled out: EDVR's GPU timestamp brackets as a sync cost, because a pair
  around every one of 100 real draws costs 0.002 ms per batch.

Also read from the same dump, for the shimmer arc: `eye capture: 151043
history hidden -- NVIDIA's lookup invalidated at 28 of 5837076 pixels
(0.000% of the eye) on scene frame 11476` -- the guard's census, exactly
the offline replica's 0.0003%.

### 2026-09-17 (later) -- a17c793 flown, the dispatch ruled out, the hook priced offline

Flights 140400 and 141246 (Frontier, a17c793, 65%, 2646x2206 in): over
terrain benchmark w6 cpu 5.086/13.748/16.680, gpu 11.084/12.366/14.085
ms; `terrain motion GPU: 85024 patches (85024 original draws, 0
reissues) ... 16.966 us/patch ... Batched build: 1742 batches, 12.309
us/eye`; app render 9.9-10.7, producer 10.0-10.7, door prep 0.33 + full
3.06-3.14 + ui 0.36. Identical to the pre-batching numbers. Mesh motion
over terrain: zero coverage reissues (its count froze at 746,643 once the
station was left), so the ships path is not in the terrain frame at all.

- ruled out: the per-patch `Dispatch(1,1,1)` as the carrier, because
  removing it (a17c793) left every frame-level number where it was, and
  the micro-benchmark prices the dispatch at 3.1 us a patch (0.3 ms a
  frame) against a 2.2 ms gap.
- ruled out: the every-64th-draw bracket's 13-17 us as the hook's cost,
  because the same bracket read 12.8-17.4 us with the dispatch gone; a
  timestamp pair around one draw measures pipeline latency (or GPU idle
  while the render thread is elsewhere), not throughput.
- ruled out: `advanced.texture_lod_bias = auto` (-0.62 at 65%) as the A/B
  delta, because it is fixed at hook install from the ini and never
  re-applied on a live `fix.temporal_aa` flip, so both sides of Sean's
  live A/B carried the same samplers. It remains a constant cost worth a
  launch-level A/B of its own (both installs run it under any temporal
  mode).
- ruled out: the pixel shader + 2 MRTs bound over the game's null-PS
  prepass (the depth-only fast path lost) as more than ~0.15 ms a frame,
  measured below with terrain-shaped geometry.
- ruled out: the hook's D3D11 calls on the render thread as the CPU rise,
  measured below at 0.3-0.4 us a patch for the faithful begin()/end()
  sequence (through the runtime and NVIDIA's driver, not through EDVR's
  own wrappers -- that part only a flight can price: `Hook CPU` above).
- ruled out: the game blocking in GetData, because the `game query` probe
  sees ~13 polls a frame with flags=1 and S_FALSE simply returned ("no
  extra poll or flush").

Micro-benchmark 1 (scratch `hookbench.cpp`, RTX 5090, 2646x2206, depth
cleared to 0 under LESS so the draws themselves filled nothing -- the
state, copy and dispatch costs stand, the "PS bound" figure does not),
GPU us per draw at N=100/400: 2 RT switches 0.10/0.15; 3 copies
2.83/2.91; UpdateSubresource(48 B) 0.00; Dispatch with CS churn
3.05/3.08; the old hook 5.05/5.14; a17c793's hook 3.16/3.25.
Calling-thread CPU 0.1-0.2 us a draw for any of them.

Micro-benchmark 2 (scratch `terrainbench.cpp`, same GPU, 100 patches of
GxG quads, 4% of the eye each = 4x overdraw, GPU ms per batch of 100):

| shape | 64x64 f-to-b | 64x64 b-to-f (23M frags) | 256x256 f-to-b | 256x256 b-to-f |
|---|---|---|---|---|
| depth-only prepass | 0.085 | 0.123 | 0.545 | 0.546 |
| PS + 2 MRT bound once | 0.103 | 0.269 | 0.555 | 0.606 |
| PS + 2 MRT switched per draw | 0.105 | 0.271 | 0.560 | 0.611 |
| + UpdateSubresource(b13) per draw | 0.105 | 0.272 | 0.564 | 0.611 |
| + 3 CopySubresourceRegion per draw | 0.662 | 0.844 | 1.226 | 1.324 |
| a17c793's hook (all of it) | 0.668 | 0.875 | 1.236 | 1.355 |
| ...with begin()/end()'s Get/Set calls | 0.670 | 0.879 | | |

The copies cost a fixed 5.6-7.2 us per patch between real draws (2 us
each, a copy-engine sync, not a drain: it does not grow with the draw's
weight), the PS+MRT binding 0.02-0.15 ms per 100 patches, the b13 update
nothing, the render-thread calls 0.3-0.4 us a patch. At 97 patches a
frame the hook is ~0.65 ms of GPU; the old dispatch shape was about the
same, hence the flat flight.

What the A/B log says about the CPU (124139, `native timing CPU` and the
benchmark windows): EDVR's door CPU is submits 0.6-0.9 ms, temporal
0.1-0.2 (dlss) or 0.014 (off), the same on both sides; the game's frame
CPU is 0.76 ms in orbit under dlss, 2.8 over terrain under off, 4.4
under fsr (GPU 9.3 of an 11.1 ms budget, not saturated, so not
back-pressure), 4.7-5.2 under dlss (GPU 10.9-11.0). A CPU rise that
follows the GPU frame's length under an unchanged hook set is a wait on
the GPU inside a call the benchmark does not subtract. Map is the one
Elite's terrain would make (GPU-generated terrain, heights read back for
physics); hence the instruments in the Status block.

### 2026-09-17 -- read from logs 122915 and 124139, no new flight

The 12:29 flight (guard build, 75%) was GPU-bound in flight: benchmark gpu
p50/p95/p99 11.7-12.8 / 14-18 / 28-40 ms, cpu 6.6-8.0 / 15-20 / 32-38,
63-78 fps, `producer` 11.5-14.9 ms against 11.1. The two earlier Pimax
Frontier sessions today (09:15 at 85%, 10:55 at 65%) never reached
`journal: LoadGame` -- main menu only -- so they do not bracket the game's
cost over terrain; the menu at 12:29-12:31 ran at 13 fps (`wait 75 ms` in
the runtime) and is not the complaint either.

Sean's A/B (12:43-12:44, 65%) is the evidence: six benchmark windows
across live toggles, and the `Application-render GPU` sequence 4.4-4.6 ms
(off) against 10.2-10.7 (dlss) over terrain, with the door brackets at
3.6-3.8 per pair. The missing 2.2-2.4 ms is under nothing EDVR times at
the door; it appears when the terrain path engages (12:43:25, "replacement
compute shader compiled") and not in space before it.

Where the per-draw hooks stand, per frame, from the same logs:

| hook | per frame | per call (GPU) | measured how |
|---|---|---|---|
| terrain patch (celestial_motion.cpp) | 100 (low flight), 86-98 (landed) | 13.5-15.8 us incl. the game's draw | `terrain motion GPU` bracket, every 64th draw |
| cockpit (holo_motion.h prepare) | 20 per eye | not bracketed | `holo motion: eye run ... (20 total)` |
| ships (mesh_motion.cpp, batched) | 42-47 coverage reissues | 1.7 us; build 17-24 us per eye | `mesh motion GPU` |
| stellar ring (ui_depth.cpp) | 7.5 | 22.3 us | `stellar coverage GPU` |
| interface depth reissues | 33 | small draws | `ui depth: engaged` |

Ruled out along the way:

- ruled out: the terrain history guard (8dd5c07) as the carrier, because
  the door brackets that hold the mv pass read prep 0.46-0.57 ms, level
  with the pre-guard flights (0.50-0.53 at the same output), and the guard
  adds 16 groupshared reads per pixel and nothing per draw.
- ruled out: NVIDIA's own DLSS cost as the whole story, because `full` is
  2.8-3.0 ms per pair in every flight today and the A/B gap is 6 ms.
- ruled out: the depth/luma probes as the delta, because their line counts
  are the same in the 09:15, 10:55 and 12:29 sessions (357/402/434 depth,
  243/279/175 luma) while only 12:29 was over terrain.
