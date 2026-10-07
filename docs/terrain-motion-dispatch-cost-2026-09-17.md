# DLSS doubles the frame over terrain (2026-09-17)

## Status

- State: RETIRED 2026-10-01. `advanced.terrain_motion` and its per-patch hook
  are deleted (Sean: "Build the jitter change and remove terrain motion
  now"). Terrain pixels take the camera's motion, exactly what the key off
  gave them: tools\terrain_retired_test proves it byte for byte on WARP. On
  branch claude/jitter-phases-terrain-retire (commit 1 of 2), gated and NOT
  FLOWN; the regression flight is in the top journal entry.
- Why it could go: the hook predates engine-record motion, whose camera
  rows now give the camera term; landed, the two agree to 0.002 px rms;
  Sean's low-flight A/B, key off against on: "no smearing" either way; the
  hook cost GPU p50 +0.27 ms, p95 +0.48 ms and CPU p50 +0.51 ms (Frontier
  log edvr_gfx_20261001_090933.log, interleaved toggles); and it still
  matched build 332841 (62/62 patches), so a retirement, not a fix.
- Tested 2026-10-06, and it fails in supercruise: Sean saw a planet blur
  on approach (dump eye_180540, eccfce7a). Its patches grew 7% in area in
  15 frames (0.40 render px a frame at the edge). The motion DLSS got over
  them carried under 1% of that: the v/d error the theory predicted.
  Details are in the top journal entry. It is not a regression: Sean's
  Frontier ini had the key off.
- Next: FIX DESIGNED 2026-10-06 (Sean: "Let's fix it properly"). Each
  body's rigid motion is taken from its patch constants and replaces the
  camera term on that body's pixels. Design and evidence are in the top
  journal entry; implementation is in progress. Sean: the blur shows at
  approach speeds near or above c. The low-flight check and the
  fast-rotating-body orbit are still unflown.
- The rest of this doc is the 2026-09-17 arc that priced the hook, kept as
  the record. FLOWN 16:34 (log 163420, build 91d5b75, Pimax Crystal Super,
  2646x2206 in, DLSS out 4072x3394): (a) CONFIRMED, benchmark gpu p50 under
  dlss 9.0-9.6 ms against 10.2-10.6 on 185ceee at the same spot; the
  per-patch bracket 3.5-4.0 us against 13.5-15.8; the 306 terrain copies
  gone; the CPU shadow served 256856 slots against 103 GPU copies;
  `history hidden` 0.000% of the eye. The key off under dlss moved nothing
  (the residual was below noise) and made the terrain flicker, as it must.
- ATTRIBUTED (flight 150849 on 185ceee): the gap was the SUM of EDVR's
  per-draw GPU syncs inside the game's passes: +306 CopySubresourceRegion +
  102 UpdateSubresource (terrain, 51 patches/eye), +37 Dispatch(1,1,1)
  (holo) + 10 (stellar ring), ~50 mesh/probe copies, +38 query Ends. Offline
  prices on the 5090: a copy ~2 us fixed, a Dispatch(1,1,1) with CS churn
  4.75 us, a timestamp pair free.
- Left inside the game's passes under dlss (flight 163420, per frame): holo
  33 Dispatch(1,1,1) + 39 UpdateSubresource + 24 instance copies; stellar
  ring 10 dispatches; mesh 10 copies + 10 uploads. By the offline prices
  0.3-0.45 ms a frame: (b) batch the holo dispatch and (c) the stellar
  ring's, a rig and a flight each, still Sean's call.
- The rest of "DLSS doubles the frame" is the door itself: NVIDIA's `full`
  2.8-3.1 + `prep` 0.2-0.5 + `ui` 0.36-0.6 = 3.5-4.0 ms per pair at
  4072x3394, a per-output-pixel price. Levers: the DLSS rectangle (the
  foveated arc, paused by Sean) or a smaller game render.
- ruled out (by measurement, journal): the per-patch dispatch (a17c793
  flown flat); the game's Map stalling on the GPU (0.003 ms/frame); the
  hook's CPU (0.03 ms/frame); EDVR's timestamp brackets (free); the
  texture LOD bias; the PS + 2 MRTs beyond ~0.15 ms; write-combined mapped
  memory. Open: the benchmark's CPU figure under dlss (3.9-4.2 on the
  planet, 1.9 after the runtime's wait moved into Submit).
- Report: Sean, Frontier, Pimax Crystal Super, after the terrain history
  fix (docs/terrain-history-shimmer-2026-09-17.md): "performance is now
  quite bad flying low to the surface" (log 122915, HMD quality 75%), then
  his A/B in log 124139 at 65%: "DLSS effectively doubles the gpu and cpu
  frametime numbers" (off cpu 2.8 / gpu 4.5-4.9; dlss 4.7-5.2 / 10.9-11.0;
  fsr 4.4 / 9.3). The oldest journal entry has the reading of those logs.

## Journal

### 2026-10-06 (later) -- the design: each body's rigid motion from its patch constants

This entry was measured offline, with no flight. Inputs:
- `eye_180540`;
- `drawstate_180540.bin`: 1368 draws each of the two patch VSs over 19
  frames, with VS b0/b1/b2;
- 88 older Frontier dumps, read by a research subagent.

**How a patch is placed.** From disassembly of `vs_72BDD292154158AD`:

    X = rotate(q = cb2[10], lerp(cb2[4], cb2[5], h)) + cb2[2]

- X is camera-relative, in head axes.
- `cb0[9..11]` is the camera rows' rotation (equal to EDVR's rows once the
  dump's frame counter is offset by one).
- `cb1[270..273]` holds per-eye clip columns, jitter included.
- The depth pass `vs_ACE405F428C17EF6` has the same structure with
  q = `cb2[8]` and t = `cb2[1]`. That is the retired hook's capture point.
- Each body is a cube-sphere: 6 face patches at this range, its radius in
  `cb2[12].w`. 36 draws an eye a frame are 6 bodies, including bodies 470
  to 810 Mm away.

**Supercruise loses only the ship's translation.**
- In the camera rows' world-aligned frame, every body moves by the same
  vector each frame: 2.5-2.8 km on calm frames, 25-28 km on slow ones. So
  the frame is world-aligned and travels with the ship. Rotation is right;
  the ship's translation is missing.
- The research subagent read 88 older dumps:
  - In normal space the rows carry the ship's translation (41 dumps,
    median 2 m a frame).
  - In supercruise the rows freeze at the ship-centred eye point, about
    13.5 m (44 dumps).
  - The retirement's 0.002 px agreement came from landed and hovering
    dumps.

**The world path's prediction matches the dump.** EDVR's world path holds
the moon still in that frame. Its prediction matches the dump's MVs exactly
once the jitter step is removed; for example, at 23651
(2.124, 0.433) - (1.874, 0.766) = (0.25, -0.333). What is left at the
moon's centre is the missing motion: about 0.9 render px a frame steady,
and 3.6 px on slow frames.

**One rigid delta per body.** Each face gives
`D = T_prev T_cur^-1`, with `T = [A R(q) | A c]`. All 6 faces agree to
1-2 m in translation, with rotation about 0. In the old landed
`Terrain.bin` dumps, every patch's delta agreed too (rotation within
1.3e-6, translation within float32 rounding of |c|). So one matched patch
gives the body's motion, including the LOD patches that are new this frame.

**The design.**
- **Capture on the CPU.** The retired CPU shadow tee (91d5b75) is restored,
  scoped to the patch constant blocks. It records each patch's c, q,
  static box rows (its identity across frames) and the body radius. There
  are no per-patch GPU copies, reissues or extra draws.
- **One transform per body, per eye and frame.**
  - Patches are clustered into bodies by radius.
  - Each patch is matched to last frame's by its static rows.
  - The body's delta is the consensus of its matched patches, taken from
    the nearest patch first, because float32 error grows with |c|.
- **Coverage.** A pixel belongs to a body if its depth falls in the body's
  depth interval and the pixel lies in its screen rectangle, both taken
  from the patch boxes. No coverage pass is drawn.
- **Motion shader.** World-path pixels with finite depth inside a body's
  volume use the body's `[R|t]` (`A_prev^T D A_cur`) in place of the
  camera's, under a new decision path 12, `celestial`. It replaces the
  camera term rather than adding to it, so it is right whether the rows
  carry the ship (normal space) or not (supercruise), with no regime test
  and no jump gate in its way.
- **Fallback.** With no matched patch the pixel gets the camera term, as
  today.

**Known residual.** The VS's distance-based LOD morph (`cb2[0..3]`) moves
vertices non-rigidly, and no rigid transform covers that.

**Verification.**
- A rig over the real dumped constants (19 frames, both eyes) must:
  - reproduce the measured deltas within 2 m;
  - reproduce the moon centre's true jitter-free motion within 0.01 px of
    the scratch scripts.
- Then one flight: a fast approach to a planet, near or above c, with an
  eye dump.

### 2026-10-06 -- the untested case, tested: a supercruise approach blurs the planet

Sean asked for a VR regression check after the DLSS performance review. The
setup:
- Frontier install, build `v0.18.2-56-geccfce7a`.
- Pimax Crystal Super.
- DLSS rendering 2016x1949 to 4032x3898 per eye.

He reported "planet was blurring on approach in supercruise". Eye dump
`eye_180540` caught it at 18:05:40, approaching 38 Lyncis 4 F at 3.9 Mm.

**The planet was not EDVR's tracked planet draw.**
- The traced frame has 108 draws each of two passes:
  - `ACE405F428C17EF6`: depth, no PS.
  - `72BDD292154158AD`/`76849D64AC657DB9`: colour, n=2304.
- This is the patch renderer the retired hook keyed on
  (`celestial_motion.cpp`'s `kTerrainDepth`).
- There is no draw of `kPlanetSurfaceVs` (`71DD9863DCFC0986`).
- The GPU census's `planet` row reads 1.5-1.7 draws a frame in the two
  windows before 18:05:14, and none in the dump's window.

**Every planet pixel took the world path.** Over the disc the decisions read:
- world 1.000;
- depth valid 0.80-1.00;
- Z 3.2-3.7e6 m.

**Measured from the 16 D crops** (a scratch script, box x720-1130
y430-850):
- The disc's depth silhouette grew from 88657 to 94839 px over 15 frames,
  +6.97%. That is an isotropic scale of 2.32e-3 a frame, or 0.40 render px
  a frame at the edge.
- A least-squares affine fit of the motion handed to DLSS over the same
  pixels:
  - isotropic divergence -1.7e-5 per px on average;
  - at most 2.1e-4 in any single frame, with either sign;
  - fit residual under 0.02 px.
- So the camera term carried head rotation and under 1% of the approach.

- ruled out: the camera term carries a supercruise approach, because the
  measured growth is 100 times the motion's divergence (above). The
  retirement's theory held: the error is about v/d a frame, here 2.3e-3.

**Not a regression.**
- `planet_motion.h`, `holo_motion.h` and `ui_depth.cpp` are unchanged
  since `521d625b` (Sean's previous Frontier build) and since `v0.18.2`.
- Sean's Frontier `edvr.ini` carries `terrain_motion = off` (line 1092), so
  the patches got the camera term before the retirement too.

**Options, not built (Sean's call):**
1. Give the patches the body's draw-transform motion: the planet path's
   affine approach, extended to the patch pair, with coverage taken from
   the depth pass. The retired hook did this per patch at GPU p50 +0.27 ms
   and CPU p50 +0.51 ms. One transform per body should cost less.
2. Find why the camera term carries no approach in supercruise, though it
   matched the hook when landed (0.002 px) and showed no smearing in low
   flight. Then fix it once, for every body.

### 2026-10-01 -- advanced.terrain_motion RETIRED: the hook, its shaders and its rig are deleted

Sean approved it on 2026-10-01 ("Build the jitter change and remove terrain
motion now"). Branch claude/jitter-phases-terrain-retire, commit 1 of 2 (the
second is the jitter phase count, docs/design-flat-temporal-aa-2026-09-23.md
section 84). Gated by the full build, NOT FLOWN.

The evidence for it, all from before this edit:

- The hook predates engine-record motion
  (docs/kinematic-motion-injection-2026-09-19.md). The camera term now comes
  from the game's own camera rows, so the hook's reason to exist, a vector
  for the terrain the camera model got wrong, is gone.
- Landed, the camera term and the hook's motion agree to 0.002 px rms.
- Sean's low-flight A/B, the key off against on: "no smearing" either way.
- The hook's cost with it on (Frontier log edvr_gfx_20261001_090933.log,
  interleaved toggles): GPU p50 +0.27 ms, p95 +0.48 ms; CPU p50 +0.51 ms.
- It still matched on game build 332841 (62/62 patches), so this is a
  retirement, not a fix for something broken.
- Untested: a glide or an orbit beside a fast-rotating body. Theory puts the
  camera term's error at about v/d per frame, about 0.5 px at 10 km for
  465 m/s. Sean accepted that.

What went, each item checked against the code before it was deleted:

- src\d3d11\celestial_motion.{h,cpp}: the per-patch hook (the draw-time copy
  of each terrain patch's three constant-buffer segments, the private coverage
  index and depth, the batched build dispatch, the CPU shadow of the three
  buffers, the per-eye record buffers, the eye-dump snapshot), the three
  shaders it compiled (kCelestialBuildHlsl; kCelestialIndexHlsl and its
  `original` entry) and their six rows in tools\temporal_shader_build (three
  variants, three source-hash contracts; the pinned counts 33 and 58 are 30
  and 55).
- Its tees: vscreen.cpp's Map, Unmap, UpdateSubresource, CopyResource,
  CopySubresourceRegion and ExecuteCommandList writes, device_hook.cpp's
  CreateBuffer one, the draw hook's capture (null-PS prepasses) and reissue,
  the boundary tick `celestial_motion`, its shutdown and the live read of
  `advanced.terrain_motion` at temporal_pass.cpp:6139.
- The motion-vector shader: terrainPixel(), TI/TZ/TR (t9..t11) and both call
  sites, decision path 7 in `mv` and the world=1 override in fetchHistoryT.
  probe.w bit 8 and the trace's inputs bit 32 are retired, not reused; t9..t11
  are free in the three SRV arrays.
- The eye dump's TerrainIndex and TerrainZ inputs (slots 5 and 6 keep their
  numbers as "(retired 5)" and "(retired 6)": the numbering is preserved on
  purpose) and the `Terrain` record file (EDVRTRN1).
- The GPU bracket and its lines: `terrain motion GPU: ...` and the six
  `terrain motion: ...` notes; in the 30 s census the in-frame item `terrain`
  and the altered-draw class `terrain prepasses (EDVR's motion target and
  shader)` (GpuCensusSection::FrameTerrain, AlteredTerrain,
  AlteredDrawClass::TerrainOriginal; the rotation's cycle is 20 turns, not
  22); the door's `hologram resolve+celestial` is `hologram resolve`.
- tools\celestial_motion_test, tools\terrain_motion.py and build.bat's
  :rig_terrain_motion: about 104-115 s, the pool's wall
  (docs/build-time-2026-09-29.md, item 2). tools\terrain_retired_test
  replaces it at about 20 s.
- The ini block and, with the reader gone, the developer-tier row of the
  in-headset menu (the tier lists every key the code reads).

Kept on purpose: tools\eye_decisions.py still names path 7 `terrain` and
tools\eye_inputs.py still reads format 42, so an older dump reads; both say
retired. The planet and stellar motion hooks, mesh motion's retired key and
the engine-record paths are separate things and untouched.

What terrain gets now. Every terrain pixel takes the camera term, which is
exactly what the key off gave it before this edit; nothing else in the shader
changed (two comments aside). tools\terrain_retired_test is the proof, on WARP:
it rebuilds the pre-retirement shader from the current text and the removed
fragments (verified byte-identical to the file at 3f226fcb with --verify-old:
82943 characters, FNV-1a-64 1e62c5d0fcb7b3c2), compiles both the production
way (cs_5_0, flags 0, diagnostic and fast variants of `mv` and `main`, twelve
compiles on twelve threads) and runs 316 configurations over three
terrain-carrying scenes (an index texture, a matching depth and a record
buffer bound to both): knobs.y, tvCam.w, probe.w {0, 1, 2, 128, 6, 7, 132},
the debug views. Every UAV (O, N, Stats, MV, ZC, MK, UN, ML) is
compared bytewise, the new shader twice per configuration for WARP's
determinism: byte-identical in all. Non-vacuity: reflection shows the
reference binds TI/TZ/TR where the new shader binds nothing, with cbuffer P
identical; the reference with bit 8 SET moves only the pixels the old gate
admitted (TI != 0, depth matching), to the motion a CPU model of the old
function gives, and the new shader ignores the bit; and four one-token breaks
of the camera term (the head path's and the world path's translation, in both
entries) are caught in every configuration where the line runs and in none
where it does not. 1547 checks, 20 s.

An old ini that still carries the line. The installer's merge keeps it:
`terrain_motion = off` (live, from a rig that flew the A/B) is carried into
[advanced] under "# carried over from your edvr.ini; this version no longer
uses it", reported as retired, not adopted and not eaten, the documentation
block is not resurrected, and a second merge does not duplicate it; a rig
that left it commented carries nothing. At run time the config audit names
it in the log as a setting this build does not read ("a typo or a retired
setting -- those lines do nothing"). tools\installer_test pins the merge,
tools\config_test the key's absence from the shipped ini.

The flight (a regression check, not a measurement of a gain). Install the
build, verify the log's build line with
`python tools\edvr_log.py --target frontier --expect-build HEAD`, DLSS on,
the HMD quality of the 09:09 flight, one low flight over a planet's terrain,
and if it is cheap a glide beside a fast-rotating body.

- Pass: no smear or ghost on terrain that moves against the camera, by eye
  and in an eye dump (`history hidden` in the dump's MV census near 0.000%,
  what the 163420 flight read); no `terrain motion` line in the log; the 30 s
  GPU census line has no `terrain` item and no `terrain prepasses` class;
  the benchmark's GPU p50 over terrain at or under the key-off figure of the
  09:09 flight, the hook's +0.27 ms gone.
- Fail: visible terrain smear or ghosting at speed beside a rotating body (the
  untested case: then the camera term is not enough there and the hook, or
  its motion from the engine's own record, comes back as a decision, not as
  a quiet revert), a `history hidden` rise above a few tenths of a percent,
  or any `terrain motion` line (the log would be from a stale build).

### 2026-09-17 (night) -- flight 163420 on 91d5b75: (a) confirmed, the residual priced

Frontier, Pimax Crystal Super, parked on the planet, 2646x2206 in and
4072x3394 DLSS out (4100x4049 headset), build v0.17.0-rc.3-107-g91d5b75.
Two draw censuses: census 1 at 16:36:00 under aa off, census 2 at
16:36:17 under dlss (with the eye dump 163617). dlss on at 16:36:08;
`advanced.terrain_motion` off 16:36:26-16:36:47 (the settings panel,
live); the session ended 16:37:46. No aa-off benchmark window on the
planet: windows 1-5 (off) are the menu and the approach, and the 4.29 ms
app render at 16:37:45 is the exit frame (luma 0). The off reference
stays the earlier flights' 4.4-5.1 ms.

| reading | 185ceee (flight 150849) | 91d5b75 (this flight) |
|---|---|---|
| benchmark gpu p50 under dlss | 10.2-10.6 ms (w10-12) | 9.02-9.62 ms (w6-13) |
| benchmark cpu p50 under dlss | 4.6-4.8 ms | 3.9-4.2 (w6-12), 1.9 (w13) |
| `Application-render GPU` under dlss | 10.0-10.4 ms | 8.8-10.3 ms |
| door per pair (prep + full + ui) | 0.33-0.46 + 2.62-3.13 + 0.36 | 0.22-0.53 + 2.79-3.14 + 0.36-0.61 |
| terrain per-patch bracket | 13.5-15.8 us (a17c793 flights) | 3.47-3.96 us |
| `Constants:` | -- | 256856 CPU / 103 GPU / 3 re-watches |
| `tee copies` | -- | 0.03 us each, 0.010-0.017 ms/frame |
| `history hidden` | 28 of 5837076 px | 25 of 5837076 px |
| terrain_motion off, gpu p50 | -- | 9.02-9.62 ms (w8-11), no change |

So (a) took about 1.0 ms a frame off the GPU at the same spot, which is
the top of the -0.6..-0.9 expectation, and the hook is now below the
noise: turning it off entirely moves nothing (and the terrain flickers,
as the lever must -- off hands the terrain the camera's motion vectors,
which is the shimmer arc's complaint returning). The captured constants
are exact: a wrong row would put the terrain's reprojection off and the
history-hidden census would climb from 0.000%; it did not. The tee's
memcpy of 128-224 bytes out of the game's mapped buffer reads 0.03 us,
so the mapped memory is not write-combined for this driver and the
scratch-pointer lever in the previous entry is not needed.

The census diff, census 2 minus census 1, frames 1-2, per frame
(scratch census_diff.py): eye draws 550 vs 540, offscreen 712 vs 657-671,
copies 1740 vs 1431-1471, dispatches 131 vs 49, query begin/end/clear
148-160 vs 83-84. The terrain hook's three `copy S buf {208,5376,288}
-> inputs` per patch are gone (the game's own copies of those buffers
read +12/+10/+4, noise); what dlss still adds inside the game's passes:

| excess per frame | what | offline price |
|---|---|---|
| +106 `copy U -> buf 48` | terrain g_draw per patch | 0 (48 B UpdateSubresource) |
| +33 `dispatch cs=58CB1128... n=1,1,1` + 39 `copy U -> ?` (96 B) + 24 `copy S buf 32768 -> ?` | holo hook per draw: draw CB, instance copy, dispatch | ~0.16 + ~0.05 |
| +10 `dispatch cs=ED270307... n=1,1,1` | stellar ring per draw | 0.05 (sync) .. 0.2 (its own bracket) |
| +10 `copy S buf 32768 -> buf 1048576` + 10 `copy U -> buf 49152 stride=96` | mesh motion instance store + keys | ~0.05 |
| +48 `clear E` | query Ends (EDVR's timestamps) | free |
| +3 + 3 `dispatch n=1,1,6` | door-side builds | at the door |

That is 0.3-0.45 ms a frame by the offline prices, 3-5% of a 9.2 ms
frame, and it is all that is left of EDVR inside the game's passes. The
256x256 BC6H mip chain (18 + 36 copies, 12 draws) that sat in the OFF
census last time sits in the DLSS census this time: the game's probe
update, caught by whichever census ran over it, not a dlss cost.

The remaining doubling is the door: NVIDIA's `full` 2.8-3.1 ms per pair
plus EDVR's `prep` 0.2-0.5 and `ui` 0.36-0.6, 3.5-4.0 ms a frame at
4072x3394 whatever the scene holds. Sean's loading screen -- off ~1.2
ms, dlss over 5, a black frame with a spinning model -- is the same door
on nothing, and says the same thing: the price is per output pixel. The
levers are the DLSS rectangle (the foveated arc, paused at the quantum
fix) or a smaller game render behind the same output.

Noted, not chased: at 16:37:05 the runtime's per-frame wait moved from
xrWaitFrame (`wait 4-5 ms, submits 0.7`) into Submit (`wait 0.1,
submits 8.3`), the benchmark's cpu p50 fell from 3.9-4.2 to 1.9 with
the GPU unchanged, and `ui` rose from 0.36 to 0.60 per pair. A pacing
change in the runtime or a menu left open; it does not touch the GPU
figure this arc is about.

- ruled out: the mapped constant-buffer memory being write-combined on
  this driver, because the tee's 128-224 byte memcpy reads 0.03 us.
- ruled out: any residual cost in the terrain hook worth a flight,
  because `advanced.terrain_motion = off` under dlss reads the same
  9.0-9.6 ms as on (windows 8-11 against 6-7 and 12-13).

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
