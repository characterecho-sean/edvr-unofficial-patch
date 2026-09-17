# Settlement flicker + frame time (2026-09-17)

## Status

- State: `eye_165144` read offline, no flight taken, no code changed.
  EDVR's treated output does NOT flicker in the dump: aligned per band,
  all 15 frame pairs sit inside the confirmed-good 15:10 baseline's range
  and the output follows the raw camera motion at x1.54. The first read
  (journal, first entry) said the opposite: a whole-frame alignment had
  locked onto the static SRV cockpit while the world moved 10 px. The
  leading explanation for what Sean saw is downstream of EDVR: every
  frame at the settlement is late (gpu p50 13.4 ms against the 11.1 ms
  refresh), the runtime holds each frame for two or more refreshes and
  reprojects; lit buildings against a black sky judder while the SRV or
  head moves. A dump cannot see that; a no-build flight can (Next).
- Report: Sean, 16:51:44 Frontier/Pimax Crystal Super over Pimax OpenXR,
  in the SRV at a planetary settlement on the body of this morning's
  terrain shimmer: "quite poor frame times and consistent flickering of
  the buildings". Log `edvr_gfx_20260917_164722.log` (`--nth 1` since the
  17:00 Quest 3 flight; `--expect-build 91d5b75` exits 0), build
  v0.17.0-rc.3-107-g91d5b75, DLSS K, 2646x2206 in, 4072x3394 out (trims
  live). Native log `edvr_openxr_20260917_164724_110_17040.log`.
- What the dump does show:
  1. Per-band alignment (journal table): T follows C x1.54 in every pair;
     T mean|diff| 1.2-4.7 against the still baseline's 0.5-4.9. History
     hidden 0.025%. `motion.csv`: dlHistory=1, jumped=0 on all 16 frames;
     dt 11.1 nominal before the capture, 36-42 ms during it (its stall).
  2. The mesh path is not under the buildings: of 512 records/eye, 81
     cover any pixel, 431 none (427 with origins in view: occluded or
     sub-pixel clutter); 626k of 669k covered px are 8 invalid records at
     0.4 m (cockpit, ship path); world geometry on the path is 73
     records, 0.74% of the eye. Both eyes at the cap every frame (1,023
     captured/frame), refusals uncounted (instrument gap). Match 36/504;
     483 records share keys 0..4 (the fuzzy matcher's ambiguity: real,
     not the buildings' problem).
  3. Frame time (windows 7 -> 10): cpu p50 3.8 -> 9.0 ms, gpu 9.0 -> 13.4
     (p95 21), fps 90 -> 41. Game draws/frame 570 -> 19,675 (34.5x); EDVR
     copies 2.7x, dispatches 3.3x. Mesh path/frame: 40 -> 212 reissues
     (~0.6 ms), 10 -> 85 capture dispatches (~0.5-0.8 ms with the in-pass
     break); terrain hook CPU flat 0.036 ms. Native: wait_frame p50 9-10
     ms (w1-7) -> 0.22 ms (w8-10): late every frame; pacing=0 (SRV).
- Hypotheses, with signatures:
  H-runtime (leading): late frames reprojected by the runtime; signature
     in hand (wait_frame collapse, gpu p50 > 11.1). Test, no build: stop
     the SRV and hold the head still (judder needs motion, an EDVR
     flicker does not), then lower HMD quality until gpu p50 < 11 ms.
  H-game: raw flicker. C diffs 3.5-7.2 inside the baseline's 2.3-8.3.
  H-elsewhere: a period beyond 16 frames or buildings outside the centre
     crop. Only if H-runtime fails: a second dump, SRV stopped, centred.
- Ruled out: the terrain-history-shimmer mechanism, because the guard
  reads 0.025% here. Mesh-motion match failure as the buildings' flicker,
  because the buildings are not on the mesh path (0.74% of the eye is).
  "EDVR's output flickers in pairs 00-01/02-03", because per-band
  alignment shows those pairs moved 8-10 px with the raw input. A DLSS
  history reset at the capture, because dlHistory=1 throughout.
- Static-mesh exclusion (Sean's question): a static mesh needs no record,
  the camera path is exact for it, but the CPU never sees the transform.
  Two-stage plan in the journal (exact-transform match + census first,
  the CPU-side skip second); worth about the mesh path's 1.2-1.5 ms GPU
  plus its unmeasured CPU. The game's 20k draws and the door are not
  EDVR's to cut.
- Next: the H-runtime flight (no build). Stage 1 when Sean says go.

## Journal

### 2026-09-17 -- eye_165144 read offline (first pass, corrected below)

Tools: `eye_run_shimmer.py`'s `shift_est`/`shifted` for alignment, a hand
decoder for `Mesh.bin` (`EDVRMSH1`, header `<8s II`, 240-byte records
`{uint4 key[8]; float4 clip[3]; float4 map[3]; float4 meta}`; meta.x =
valid at float 56, meta.w = matched at 59 -- no repo tool reads it),
`eye_inputs.py` for the EDVRTEX1 channels, `eye_bmp_to_png.py --crop`.
Crops: C 1400x1400, T 2155x2154 (not 1868), L0 whole eye; T's origin in
L0 is (958,620) by residual search, C's (623,403) by the same convention.

Q1 as first read: baseline 151043 T mean|diff| 0.55-4.95 over 15 pairs
(its worst, T12->13, >8/255 on 14.1% of pixels); 165144 T00->01 10.74 and
T02->03 11.23 with the same raw pairs at 7.16/4.18. SUPERSEDED: the
whole-frame alignment reported T shifts of 0.02/0.03 px for frames 1-2
and +8.02 at frame 3 while C moved 5-6 px at frame 1 -- the correlation
locked onto the cockpit; see the per-band table below. The per-pixel mask
built on those two pairs (6.84% of the crop; holo 28.5%, mesh 8.9%,
terrain 7.8%, uncovered 4.4% flicker rate; the 8x MV gap on mesh-covered
pixels) inherits the misalignment and is not evidence of anything. Method
note for the next dump with a cockpit in view: align per band, or mask
the cockpit, before differencing; and read the tile argmax, not the tile
centre, when localising.

Q4 (source, stands): `meshPixel()` is tried first and wins on success
(`temporal_shader_source.h:678-683`); it returns false on no coverage,
index > 512, UI-covered, stale coverage depth, or `meta.w != 1` (476-488).
`meta.w` is set only by `match()`'s `counts[0]==1` branch
(`mesh_motion_shader.h:75-85`): zero candidates and several candidates
both stay 0 and are indistinguishable in the record. Past the cap
(`mesh_motion.cpp:157`) a draw is not entered at all and its pixels
rasterise as index 0. On failure the pixel takes the camera/depth
reprojection (684-693) unless body/ship/terrain/holo claims it (705-769).

Q5 (logs, stands): census at the dump (3 frames): 59,025 draws, 16,563
offscreen, 18,842 copies, 1,072 dispatches, 42,641 truncated (the buffer
overflows at this volume); 15:10 baseline: 1,710 draws, 7,011 copies, 327
dispatches. Per frame: draws 570 -> 19,675, copies 2,337 -> 6,281,
dispatches 109 -> 357. `terrain motion GPU` at 16:51:45: hook CPU 0.40
us/call, 0.036 ms/frame, 46/46 patches matched; holo 18/18. Quest 3 log
(`--nth 0`, 17:00, VDXR, 2307x1652 -> 3072x3264): cpu p50 7.1 -> 13.4,
gpu 10.5 -> 16.3 ms at 17,638-18,878 draws -- same shape, not the Pimax
runtime's doing.

### 2026-09-17 -- review: alignment, record visibility, native pacing

Per-band phase correlation (four horizontal bands, top to bottom; dx,dy
in each crop's own pixels; scratch `band_shift.py`):

| pair | C (input px) | T (output px) |
|---|---|---|
| 00->01 | -3.9,+6.6 / -3.0,+5.5 / -2.8,+7.1 / -0.8,+5.9 | -5.0,+10.0 / -3.4,+7.3 / -3.4,+10.6 / +0.0,+8.5 |
| 01->02 | +1.2,+1.0 x4 | +1.3..+1.8,+1.0 |
| 02->03 | +0.8,-0.9 x4 | +1.2,-0.3 |
| 03->08 | 0.1-1.9 px steps | 0.3-1.9 px steps |

T moves with C at x1.54 from the first pair; the lower bands' smaller
x in C is the static cockpit weighting the band. Cumulative x over 8
pairs: C x1.54 = +4.0, T = +3.1 (T is the jitter-free output; C carries
the +-0.4 px jitter in `motion.csv`). Nothing holds still and snaps.

Record visibility (`mesh_visibility.py`, MeshCoverage x Mesh.bin):
records 512, valid 504, matched 36; 669,391 px covered (11.47% of the
eye); records covering >=1 px 81, >=100 px 12, >=1000 px 8, zero 431;
origins in front 509, inside the frustum 501, zero-px records inside 427;
pixels on matched records 40,811, on unmatched valid 2,366, on invalid
626,214 (records 9, 8, 3, 1, 4, 2 at clip w 0.4); 83 distinct key groups,
483 records sharing a key, 468 of them valid and unmatched.

Mesh path per stereo frame from the `mesh motion` totals (1800-frame
marks): terrain window 16:49:46-16:50:06: 40 reissues, 40 instances, 10
batches; approach 16:50:48-16:51:10: 193 / 769 / 14; settlement
16:51:10-16:51:41: 212 / 1,023 / 85 at 6.27 us/batch, coverage 2.93 us
sampled, match 21.5 us/eye. Cap line at 16:50:55.318.

Native log `native_submit_phases`: wait_frame p50/p95 9.2/10.1 (w1),
9.1/10.1, 9.3/10.0, [w4 pacing=1 on foot, pacer_block 6.1], 4.4/6.0,
5.5/6.6, 4.8/5.9 (w7), then 0.22/1.7 (w8, 16:50:58), 0.23/2.2 (w9),
0.22/0.33 (w10, 16:52:02); output 3329x3394 per eye with the trims.
`native_pacing_summary deferred=1283,synthesized=1283` is the on-foot
stretch (window 4), not the settlement.

Static-mesh exclusion, the plan (not built). The transform lives in the
game's pool buffer (VS t33, 336-byte records, indexed by the instance-id
stream at slot 0) and is read only by the capture shader
(`mesh_motion_shader.h:28-36`); the record keeps the camera-dependent
clip matrix, which is why `match()` needs the fuzzy direction/distance
test and ties on identical props. Stage 1: keep the raw pool transform
(scale, packed quaternion, position: 8 uints of the record's 11 spare in
`key[5..7]`) and match bit-identical transforms first, the fuzzy test
only for records that changed -- unique by construction, ends the
ambiguity, no cost change. The dump readback then logs how many valid
records were identical to a previous one: ~0 means the game stores
positions camera-relative and Stage 2 is dead before it is built. Same
build: a per-frame count of instances refused by the cap, a CPU timer
on `meshMotionDraw` and `flushCapture`, a per-frame draw-hook CPU total
(20k draws/eye is where the unmeasured CPU would be). Stage 2: shadow
the pool and id-stream writes at Map/Unmap and UpdateSubresource (the
terrain fix's technique; `meshMotionResourceWritten` already receives
the byte ranges), hash each draw's instance transforms against the
previous frame's set, return before the reissue when every instance is
identical: no second rasterisation, no record, no capture dispatch, and
the cap left to the movers. A readback-based skip (draw keys static N
frames ago) would be cheaper to build but hands a newly moving object
camera-path vectors for the latency -- not chosen. Visibility ranking
of the cap (tally coverage per record, deprioritise draw keys that
covered nothing) waits until the static skip has been measured.

ruled out: EDVR-side flicker in this dump, because per-band alignment
puts every T pair inside the baseline's range and T follows C x1.54.
ruled out: a history reset at the capture, because dlHistory=1 and
jumped=0 on all 16 rows of motion.csv.
ruled out: mesh-motion ambiguity as the buildings' flicker, because the
buildings are not on the mesh path (73 world records, 0.74% of the eye).
