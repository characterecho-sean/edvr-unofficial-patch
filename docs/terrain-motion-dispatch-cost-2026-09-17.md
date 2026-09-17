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
- Open hypothesis (the instrument build, this branch): the game's own
  `Map` stalling on the GPU. The benchmark's CPU rise scales with the GPU
  frame (fsr +1.6 ms at 9.3 ms, dlss +2.0-2.4 at 11.0, orbit +0 at 5)
  although the hook set is identical under fsr and dlss; EDVR's door CPU
  (`native timing CPU` submits 0.6-0.9, temporal 0.1-0.2 ms) does not
  move; the game polls its queries without waiting (`game query` probe).
  Elite generates terrain on the GPU and reads heights back for physics;
  a `Map(READ)` on that staging copy waits until the GPU reaches it, and
  the door's 3.8 ms sits ahead of it in the queue. That wait is counted as
  the game's CPU and the GPU then idles behind it: both symptoms, one
  cause, terrain-only, in step with the GPU frame.
- Instruments built (this branch, NOT FLOWN): (1) the `native timing CPU`
  line ends with `Game Map over N frames: reads R calls X ms/frame, writes
  W calls Y ms/frame, longest Z ms, S past 0.1 ms`; (2) the `terrain
  motion GPU` line ends with `Hook CPU: A us/call over C calls, B
  ms/frame`; (3) live levers `advanced.terrain_motion` and
  `advanced.mesh_motion` (on/off, default on) so the hook's whole effect
  can be priced against the frame in one flip.
- Next flight (Frontier, low over terrain, ~15 s per state, log the
  benchmark): dlss on -> `advanced.terrain_motion = off` -> on -> `fix.
  temporal_aa = off` -> dlss. Readings: Map reads ms/frame ~2 under dlss
  and ~0.3 under off = the stall is the carrier and the lever is the door's
  GPU length, not the hooks; Hook CPU ms/frame ~1.5 = the hook after all
  (its D3D11 calls through EDVR's own wrappers); terrain_motion off closing
  most of the gap = the hook's GPU (copies) plus whatever it starves.
- Fix candidates in order: (a) capture the three constant snapshots on the
  CPU at the game's Map/UpdateSubresource of those buffers (mesh_motion's
  write-hook shape) and upload once per eye -- removes ~0.6 ms/frame of GPU
  either way; (b) if the Map stall carries it, only the door's GPU length
  helps (foveated DLSS rectangle, docs/foveated-dlss-*.md).
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
