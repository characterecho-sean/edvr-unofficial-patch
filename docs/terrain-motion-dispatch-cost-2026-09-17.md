# DLSS doubles the frame over terrain: the per-patch dispatch (2026-09-17)

## Status

- State: cause found in the flight logs; fix built (terrain build batched,
  one dispatch per eye), NOT FLOWN.
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
- Cause: celestial_motion.cpp `begin()` hooks every terrain patch prepass
  draw and, per draw, saves the compute state, binds the record-build
  shader, `Dispatch(1,1,1)`, restores, then swaps render targets for the
  coverage capture. 100 patches a frame in the low flight (`terrain motion
  GPU`: 180,406 patches in 1,800 frames at 15.8 us per bracket; 86-98 a
  frame at 13.5-13.9 us landed on Steam), each a graphics-compute-graphics
  round trip inside the game's geometry pass. The same log measures the
  shapes to compare with: the ships path (mesh_motion.cpp), which batches
  its build, costs 17-24 us per EYE per frame plus 1.7 us per coverage
  reissue (no dispatch in it); the stellar ring hook, which still dispatches
  per draw, costs 22.3 us per call. 100 x (15.8 - a few us for the game's
  own patch draw) is the 1.5-2 ms; the CPU rise is the ~40 D3D11 calls per
  patch on the render thread plus driver back-pressure once the GPU is
  behind.
- Fix (this branch): the per-draw hook only snapshots the constants the
  build reads (model[4..11], scene[270..273], patch[0..13]: 416 bytes in
  three `CopySubresourceRegion`s) and swaps render targets as before; the
  records are built by ONE `Dispatch(count)` per eye when the temporal pass
  asks for the views (`celestialMotionViews(ctx, ...)`), or at the frame
  boundary if nothing asked. Records, keys, matching and the rig's checks
  are unchanged; the rig reads records after that flush.
- Instrument: `terrain motion GPU: ... us/patch bracket (copies, the
  coverage draw and restore; no dispatch ...) Batched build: N batches,
  X us/eye (...)`. Old shader: 13-16 us/patch and no "Batched build".
  Working: a few us/patch and one batch per eye per frame at tens of us.
- Next flight: the same low flight on Frontier with the same A/B (off vs
  dlss, live). Expected: `Application-render GPU` under dlss over terrain
  down by 1.5-2 ms, the benchmark gpu p50 under dlss ~9 ms at 65%, cpu p50
  down with it. If the gap outside the brackets stays at ~2 ms with the
  per-patch bracket at a few us, the hook was not the carrier and the next
  candidates are below.
- Not changed, in order of size: the cockpit (holo) hook in ui_depth.cpp /
  holo_motion.h still dispatches per draw (20 records per eye in the Steam
  dump, bounded by the ~1.5 ms in space); the interface depth reissues (33
  a frame, small draws); the depth/luma probes in the Frontier ini (DO_NOT_
  WAIT readbacks, present in every flight compared, a constant); HMD
  quality 75% (Sean's slider, 1.33x the game's pixels of the 65% flights);
  the Frontier ini's outer trim 5 (Steam: 10, 23% fewer pixels).

## Journal

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
