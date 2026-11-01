# Stereo CPU sharing: cinema vs Explorer Cam on foot (2026-10-16)

## Status

- **State (2026-10-16): analysis only.** No code, no build, no flight. Sean's
  observation: on foot in a station hangar, the scene costs about 1 ms LESS
  CPU per frame in Elite's cinema mode (one flat camera on the 2D screen) than
  in Explorer Cam (two eye cameras). The question: can the per-eye CPU be
  shared between the eyes?
- **Answer, Q1 (whose ms):** about half of it, probably more, is EDVR's own
  per-eye work. The game's share is the second eye's draw submission and its
  per-eye pass chain. No current-build Explorer Cam window exists, so the
  split today is a projection; the A/B flight below settles it with existing
  lines.
  - The only same-spot A/B on record (2026-10-12, older build v0.19.1-63,
    hangar on foot): CPU TIME 7.3 -> 11.0 ms (+3.7). EDVR's draw hooks went
    0.36-0.98 -> 1.55-2.15 ms (about +1.2), and the runtime's two eye submits
    0.24 -> 0.48 ms. So EDVR was about 40% of that delta and the game about
    60% (about 2.2 ms). Part of the game's share was the second eye's
    light-grid build, which EDVR now caches.
  - Current build (v0.20.0-rc.2-12-g8f50e8ad, 10-16 baseline), cinema only:
    CPU TIME 6.5 ms, draw hooks 0.40 ms over about 980 hooked draws a frame
    (0.41 us a draw). Stereo with AA on runs at 0.55-0.60 us a draw (aboard),
    or 0.14-0.16 with AA off. If Explorer Cam doubles the world draws (about
    1950), EDVR's draw hooks rise about 0.6-0.8 ms. Against Sean's ~1 ms, that
    is most of it.
  - EDVR already removes the game's largest per-eye render-thread item: the
    light-grid build runs 1.0-1.2 times a frame in cinema and 2.2 in stereo,
    at about 0.45 ms a miss. The cache serves about 91% of stereo calls
    (`estimated saving` 0.85-0.95 ms a frame, 10-16 log).
- **Open hypotheses (one flight reads all four):**
  - H1: EDVR's draw hooks carry 0.5 ms or more of the delta. Signature:
    `draw hook CPU:` mean, Explorer Cam leg minus cinema leg.
  - H2: the game's second eye costs about 0.3-0.7 us per extra draw.
    Signature: (CPU TIME minus EDVR's parts) delta divided by the `perf
    baseline F5` draws-a-frame delta.
  - H3: part of the delta is Explorer Cam's own extras (third-person avatar,
    photo camera), not the second eye. Signature: Explorer Cam's per-eye
    draws above the cinema world camera's count. In 10-12, `engine motion
    CPU` calls went from 454 to about 1285 a frame.
  - H4: the runtime's per-eye submit differs between the modes. Signature:
    `first_submit_roundtrip` + `second_submit_roundtrip`.
- **Ruled out (design, see "Sharing candidates"):** union-frustum cull, shared
  LOD, a light grid shared between the eyes, render-list reuse (Elite already
  does it), and instanced stereo from a D3D11 proxy.
- **2026-10-31 (PSVR2 rig, concourse): H5, H6, H7 confirmed**
  (`edvr_gfx_20261031_225516`). The rig is CPU-bound throughout (`held_by=game`
  96%, vendor xrEndFrame 0.36 ms). In the concourse, AA on (DLAA or TAA alike)
  costs about 4.2 ms of frame time: 29.6 fps against 34.7 with F7 off, twice
  over. EDVR's draw hooks read 5.7-6.1 ms a frame with AA on and still 2.7-3.5
  with F7 off, because F7 leaves the subscriber gate open. EDVR's draw path is
  thus about 6-7 ms of a 32 ms concourse frame. See "2026-10-31: result".
- **Next flight:** a CPU profile of the concourse, AA on and F7 off
  (`cpu_profile.py`, an `EDVR_PROFILE_SYMBOLS=1` build), to name the
  functions inside those 6 ms before any change. "The A/B flight" below is
  stale: Explorer Cam and `perf baseline F5` are gone.
- **Environment:** SteamVR (aapvr), RTX 5090, 2016x2016 per eye rendered and
  DLSS'd to 4032x4032, 90 Hz; Elite build 332841. On foot, the native runtime
  paces `deferred` in both modes (the weapon-stability pacing), so the second
  submit carries xrEndFrame (about 0.55 ms) in both.

## What "CPU TIME" is

The overlay's CPU TIME tile (`perf_monitor.cpp`, native branch) is the
runtime's caller work per cycle: `native_frame_cycle_phase` `cycle` minus
`next_wait_roundtrip`, which is the game thread from one pose-wait return to
the next, submits included. It is wall time: it includes EDVR's draw hooks,
Present ticks and eye treatments, and Elite's render thread asleep in its
producer handshake (0x5D6D20; 44% of render-thread samples in the 10-11
hangar profile). Producer-side per-eye work can therefore show in it.

## Evidence

**A/B, 2026-10-12** (`edvr_gfx_20261012_044241.log`,
`edvr_openxr_20261012_044243_334_29132.log`; docked, on foot, Flags2 0x16009;
Explorer Cam 04:55:07-04:56:18). Runtime windows wholly inside one mode:

| window | mode | CPU TIME | before 1st submit | submits 1+2 | post 2nd |
|---|---|---|---|---|---|
| w25 04:54:35-04:55:05 | cinema | 7.36 | 3.83 | 0.24 | 3.28 |
| w26 04:55:05-04:55:35 | Explorer Cam | 11.00 | 7.05 | 0.47 | 3.47 |
| w27 04:55:35-04:56:05 | Explorer Cam | 10.99 | 7.07 | 0.49 | 3.43 |
| w29 04:56:35-04:57:05 | cinema | 7.30 | 3.66 | 0.26 | 3.38 |

`draw hook CPU:` (20 s windows): cinema 0.428, 0.363, 0.983; Explorer Cam
1.549, 2.148 (1.790 is mostly Explorer Cam). `engine motion CPU` p50 stayed at
0.22-0.28 ms. The cinema world screen held 2683 depth draws a frame. In
Explorer Cam the door ran DLSS on both eyes; in cinema the eyes skipped it.
The cycle rose to 12.3-12.5 ms in Explorer Cam, so the frame overran.

**Current build, 10-16 baseline** (`edvr_gfx_20261016_022808.log`, runtime
`..._022810_203_38476.log`). Cinema on foot, window 5 (02:29:57-02:30:27):
CPU TIME 6.52, before 1st 2.40, submits 0.15 + 0.80 (deferred pacing, overlap
off), post 2nd 3.18. `draw hook CPU` 0.404 ms; F5: X 935 + N 42 draws a frame.
Aboard stereo, AA on: draw hooks 0.686-0.761 at X 1098 + N 160. AA off: 0.15-0.17.
There was no Explorer Cam session in this flight.

## How Elite renders the two eyes (existing RE; build 332841)

- **Cameras.** Per-frame VR job 0x2868C30 -> driver 0x28431D0 -> composer
  0x283D4C0 -> camera setter 0x2878DC0, one eye pose times the base matrix each
  (canted-projection.md). The headset camera block is uploaded in coordinator
  0x594B60 (sole caller 0x286906E); other cameras go through view loop
  0x594D90. About 147 camera-block writes a frame into ONE constant buffer
  object (render-thread CPU arc, TCWR analysis).
- **Visibility: one traversal, a per-view loop inside.** Collection mask
  FUN_144331300 (distance/LOD per view), screen-size test FUN_144308B30 per
  view, then the draw-item builder FUN_1442B4420, whose view loop (lines
  250-275) admits records per view. The per-part test FUN_1442B3FC0 runs
  frustum + LOD on each sub-item's sphere per view and sets a view mask (eye A
  = view bit 1, eye B = bit 22). No test gives the eyes different verdicts at
  the record level. 217 parts were drawn in one eye only, each outside the
  other eye's viewport (engine-render-performance, 2026-09-22 gate probe).
  This runs on the job workers: 5.24 ms thread-summed on seven workers, 0.81 ms
  on the critical path (settlement); about 33k part tests a frame at a pad.
- **Terrain** is culled per eye against a centred frustum: 0x4455A40 ->
  0x4456007 -> plane builder 0x443E800, which EDVR's terrain cull cover
  widens. It does not apply in a hangar.
- **Light grid (fragment-culling clusters).** Builder 0x58B150 (caller
  0x590368) on the render thread, one context per eye plus third views, about
  0.45 ms a miss in a hangar. It is view-space, so each eye's grid is its own.
- **Draws.** Each eye is its own blocked pass: shaders, geometry and
  constant-buffer pointers are shared, b0 rows 4-7 are restaged with the eye's
  projection, and instanced families put eye B at startInstance +80148 in one
  shared instance buffer with an eye-independent payload
  (engine-render-pipeline stage 5). The records are re-walked about 8x a frame
  (eyes x parts/LOD x materials). Submission is on the caller thread. The
  settlement Phase A split was 4.45 ms of game code and about 2.4 ms of
  D3D/driver over about 22k draws, about 0.3 us a draw before EDVR's hook.

So the per-eye CPU is: the light grid (cached by EDVR), the second blocked
pass's submission (game + D3D11 + driver + EDVR's hook per draw), the
second eye's per-pass chain (clears, lighting, post, CB staging), EDVR's
per-eye door (DLSS evaluate, motion prep, UI resolve, HUD seeds) and the
runtime's second submit. Culling is already shared in one traversal, and it
runs off the critical path.

## Sharing candidates, ranked

1. **Make EDVR's AA-on draw hooks cheaper** (lead (d) in performance.md). Hook
   point: the `vscreen.cpp` draw thunks (kinds X and N) and their subscriber
   chain under `fix.temporal_aa`. Basis: AA-on costs about 0.4 us a draw over
   AA-off. The ceiling is about 0.75 ms over Explorer Cam's ~1900 draws;
   halving the increment is realistic (0.3-0.5 ms). Risk: a fix that loses a
   draw it subscribes to; the X thunk's post-draw consumers must still run.
   Evidence first: H1 from the A/B. The increment is unattributed today, so
   an attribution comes after H1, and it may need a per-subscriber split
   (a new instrument, not needed for this flight).
2. **Native on-foot stereo instead of Explorer Cam** (setter 5->3 arc, local
   branch). This drops the photo camera and the third-person avatar if H3
   holds. The saving is unknown: zero to a few tenths of a ms. Evidence:
   Explorer Cam's per-eye draws against the cinema world camera's, and the
   head-hide census of the avatar's parts.
3. **Light grid:** done. The residual is about 7 misses in 30 s. Reusing eye
   A's grid for eye B is wrong: the clusters are in each eye's view space and
   the off-axis frusta mirror each other, so lights at the edges would leak
   or drop.
4. **Union-frustum cull:** rejected. Elite already culls both eyes in one
   traversal; the second eye's tests are tens of ns each on the workers
   (about 0.1-0.3 ms worker CPU, at most about 0.05 ms on the critical path).
   A union frustum would make each eye draw the other's 1-eye parts (more
   draws, more GPU), and frustum edits are where the cull squares came from
   (cull-guard history).
5. **Shared LOD:** rejected. LOD is chosen in the same per-view test, and
   eyes 6 cm apart agree; there is nothing to save.
6. **Render-list reuse with per-eye constants:** Elite already does this (one
   CB object, b0 restaged, shared instance buffer). What is left is one
   D3D11 call per draw per eye.
7. **Instanced stereo:** infeasible from a proxy. Every VS family would need
   a render-target-array or viewport index and instance doubling (with the
   +80148 eye-B offset), every per-eye post pass a stereo form, and EDVR's
   per-eye DLSS, motion and UI layer a rework. It would save at most the
   game's second-eye submission (about 0.3-0.6 ms in a hangar).

The game's residual (the second eye's D3D11 submission and pass chain) is
irreducible from outside short of item 7.

## The A/B flight (minimal; existing lines only)

- Build: the installed Frontier probe DLL `v0.20.0-rc.2-12-g8f50e8ad` (it has
  the F5 per-kind draw counts). Check it first with
  `python tools\edvr_log.py --target frontier --expect-build 8f50e8ad --version`.
- On foot in a hangar, standing still, one heading, HUD and DLSS as usual. Run
  four legs of 75 s each: cinema, Explorer Cam (SCROLLLOCK), cinema, Explorer
  Cam. Each leg holds two whole 30 s runtime windows. The log marks the
  switches (`explorer cam: F5 enter`, `placed`, `released`; `world screen: the
  gate holds` for cinema).
- Optional second set, same spot, with `fix.temporal_aa = off` in the ini and
  a restart. This prices the AA-on increment; the F7 hotkey leaves the
  subscriber gate held.
- Read, per window wholly inside a leg:
  - runtime `native_frame_cycle_phase`: CPU TIME = `cycle` -
    `next_wait_roundtrip`; `first_submit_roundtrip` and
    `second_submit_roundtrip`;
  - `native_submit_phases`: `pacing=` and `frame_end_overlap=` must match
    between the legs;
  - graphics log: `draw hook CPU:`, `Present hook ticks`, the render-thread
    total of `engine motion CPU`, `perf baseline F5` (draws a frame by kind),
    and the `light grid cache: last 30 s` line (both eyes served).
- Split: EDVR = the delta of draw hooks + Present ticks + engine motion +
  submits 1+2. Game = CPU TIME delta minus EDVR. Game per extra draw = game
  divided by the F5 draws delta. `draw hook CPU` samples 1 frame in 16 and
  1 draw in 64, and one cinema spot read 0.36-0.98 in 10-12, so use three or
  more windows per leg.
- Decide: EDVR at 0.5 ms or more -> candidate 1. Explorer Cam's per-eye draws
  more than 10% above the cinema world camera's, or the game above 0.7 us per
  extra draw -> H3, candidate 2.

ruled out: union-frustum culling as a stereo CPU lever, because Elite already
culls both eyes in one traversal (FUN_1442B4420 view loop, FUN_1442B3FC0
per-view bits) on the job workers, off the critical path, and a union adds
draws.

ruled out: sharing the light grid between the eyes, because the clusters are
per-eye view space; EDVR's exact cache already serves about 91% of stereo
calls.

ruled out: instanced or single-pass stereo from the D3D11 proxy, because every
vertex-shader family and per-eye pass would need rewriting, for at most the
game's second-eye submission.

## 2026-10-31: the concourse

Environment: SteamVR OpenXR, PSVR2 at 90 Hz, 2632x2684 per eye, DLAA
(`fix.temporal_aa = dlss`, HMD Image Quality 1.0), RTX 5070 Ti, Ryzen 7
5700X3D, EDHM chained (3Dmigoto 1.3.16), Motion Smoothing forced off. Build
v0.22.0-rc.4-1-ge1606a23. On foot the frame rate is 31-38 in a concourse,
about 56 in a hangar; the runtime puts 94% of slow frames on the game
(`held_by=game`), and SteamVR reports CPU 11.4 / GPU 4.9 ms.

What the last flight already shows (`edvr_gfx_20261031_195628`, `draw hook
CPU:` windows, AA on throughout; every F7 leg was under 15 s, shorter than
one window, so no AA-off window exists):

| window ending | hooks ms/frame mean | max | eye draws |
|---|---|---|---|
| 19:57:32 (hangar) | 0.249 | 1.075 | 855 |
| 19:58:51 | 1.749 | 9.920 | 13215 |
| 19:59:40 | 4.255 | 11.059 | 15248 |
| 20:00:53 | 1.396 | 7.027 | 1132 |

Hypotheses, one flight for all of them:

- H5: EDVR's AA-on draw hooks cost 1.5 ms or more a frame in the
  concourse. Signature: `draw hook CPU:` mean, AA-on legs against AA-off
  legs, and CPU TIME (`native_frame_cycle` `cycle` minus
  `next_wait_roundtrip`) over the same legs. Refuted if the CPU TIME delta
  is under 0.5 ms.
- H6: F7 does not release the draw subscribers (the 10-16 note above says
  it leaves the gate held). Signature: `eye draws counted this frame` on an
  AA-off window; nonzero means held, and an F7 A/B then underprices H5.
- H7: the per-draw cost scales with the concourse's draw count rather than
  a fixed per-frame cost. Signature: hook ms against eye draws across the
  hangar and concourse windows.

The flight. Legs must hold one whole `draw hook CPU:` window, which is 1800
frames (about 51 s at 35 fps), so each standing leg is 2 minutes. The log
marks every F7 press (`aa compare: AA OFF` / `RESTORED`), so no timings need
writing down.

1. Concourse, standing still, one heading: AA on, off, on, off (F7 between
   legs), 2 minutes each. The repeat legs separate the AA effect from drift
   (NPC traffic, streaming, heat).
   1b. Optional, same spot: the F8 menu's Anti-aliasing set to TAA (`on`),
   2 minutes, then set back to DLSS (an F8 pick writes the ini). DLAA and
   TAA share the draw subscribers, so the hook cost should match; a
   mismatch means the per-draw price is per mode. Presets (K/L/M) and
   FSR/XeSS change GPU work only and are not legs.
2. Concourse, walking one fixed loop: AA on 2 minutes, off 2 minutes.
3. Hangar, standing still: AA on, off, on, 2 minutes each.
## 2026-10-31: result

`edvr_gfx_20261031_225516` and its runtime log, same environment as above,
build v0.22.0-rc.4-1-ge1606a23 (`--expect-build HEAD` matches). The legs were
flown in this order, each opened by a double F7 flicker as a marker. fps is
1800 frames over the spacing of consecutive `draw hook CPU:` windows; game ms
is the runtime's `native_slow_regime` `game_ms` over the leg. Eye draws is the
window's one-frame snapshot.

| leg | AA | windows ending | fps | game ms | hooks ms/frame | eye draws |
|---|---|---|---|---|---|---|
| concourse, standing | DLAA | 21600, 23400 | 29.6 | 32.0-33.0 | 5.81, 6.11 | 13.6-13.9k |
| concourse, standing | off (F7) | 27000 | 34.8 | 27.7-29.2 | 3.08 (3.47 mixed) | 13.3k |
| concourse, standing | TAA (F8) | 30600, 32400 | 30.2 | 32.0-33.8 | 5.67, 5.87 | 13.6-13.7k |
| concourse, walking | DLAA | 37800, 39600 | 39.6 | 28.0-30.3 | 3.67, 3.72 | 3.8-7.4k |
| concourse, walking | off (F7) | 43200, 45000 | 45.5 | n/a | 1.70, 1.77 | 5.7-7.4k |
| hangar, standing | DLAA | 50400-54000 | 57.4 | n/a | 1.67-1.96 | 1.9k |
| hangar, standing | off (F7) | 57600-63000 | 62.5 | n/a | 0.72-0.79 | 2.0-2.2k |
| concourse, standing | DLAA | 68400 | 29.3 | 31.7-32.8 | 5.89 | 13.6k |
| concourse, standing | off (F7) | 72000 | 34.7 | 27.8-28.4 | 2.70 | 13.8k |

GPU is not the limit anywhere: vendor xrEndFrame 0.35-0.38 ms and
`held_by=game` at 96% in every slow-regime event, so the hooks' time is
frame time.

- **H5 confirmed.** Concourse AA on against F7 off: game ms 32.3 -> 28.1
  (-4.2 ms), 29.6 -> 34.7 fps, the same in both concourse pairs. The hooks'
  own delta is 2.7-3.2 ms; EDVR's present-side work (`edvr_work_ms`) adds
  0.40. The remaining 0.6-1.1 ms lies outside the sampled draw scope; the bind
  hooks' VsSrv33/Blend shadows, which `bindShadowsSkipped` drops with AA off,
  are the first suspect (unmeasured).
- **H6 confirmed.** Every F7-off window counts 13-14k eye draws (the counter
  reads 0 on the AA-off fast path), and the hooks still cost 2.7-3.5 ms in the
  concourse and 0.72-0.79 in the hangar. Some term of `drawGateSubscribed`, or
  the VR world route, stays open with AA off; which one is not in the log. The
  live ini sets none of the probe keys (`scheduler_probe`,
  `eye_depth_capture`, `object_probe`), and `developer = on`. F7 therefore
  underprices EDVR's draw path: with DLAA it is about 6 ms of hooks plus about
  1.5 ms around them, near a quarter of the concourse frame.
- **H7 confirmed, with a fixed part.** AA on: about 0.33 us an eye draw plus
  about 1.3 ms a frame (concourse 13.6k -> 5.9 ms, hangar 1.9k -> 1.9 ms,
  walking between them). AA off: about 0.20 us a draw plus 0.35 ms.
- **DLAA = TAA** within noise (5.8-6.1 against 5.7-5.9 ms; 29.6 against
  30.2 fps): the price is the shared draw subscribers, not the AA mode.
- **Also seen:** the F100 marker-probe tee (`native on foot marker probe:
  cost`, about 16.5k tee calls a frame in the concourse) loses its own fast
  path with AA off (`fast path 0 of N`): its estimate rises from 0.47-0.56
  to 1.1-1.5 ms a frame. That is part of the AA-off floor, and backwards.

What this is worth: if EDVR's draw path in the concourse fell to the
hangar's per-draw price with AA off (about 0.2 us), the concourse frame
would drop about 4 ms (30 -> 34-35 fps with DLAA). Removing all of it would
make the frame about 25 ms, 40 fps. The game's own ~25 ms remains the floor.

Next: one profiling flight to name the functions before any change. Build
with `EDVR_PROFILE_SYMBOLS=1`, then from an elevated console
`python tools\cpu_profile.py --capture --target steam --start-key F9
--stop-key F11 --max-seconds 100 --output build\concourse-dlaa`, standing in
the concourse with DLAA, F9 then 75 s then F11; a second run with
`--output build\concourse-off` after F7. The analyzer (tools\cpu_profile,
.NET 8 SDK) then splits each leg's render thread by function, both legs,
so the AA-on delta and the AA-off floor each get their owner.