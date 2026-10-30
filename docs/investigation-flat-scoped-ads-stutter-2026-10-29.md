# Flat scoped ADS stutter (2026-10-29)

## Status

- **State: FIXED, flown (flight 4, 10-29, `97193c14`, Steam, flat, DLSS); on main
  without the instruments.** Sean: "The hitches are gone!" Six scoped presses: no frame over 29 ms (flight 3: 43 to 104 ms at every
  zoom edge). EDVR's render-thread CPU is at most 12.4 ms on any slow frame, and
  foreground capture is the heaviest family on one of them, at 2.3 ms (flight 3: 25
  to 73 ms).
- **Cause (located by flight 3, confirmed by flight 4):** inside foreground capture
  (`flatcpu::kForegroundCapture`, `FlatRuntimeDrawScope::beginActualDraw`), the
  per-object motion history (`AnimatedVertexHistory`, `animated_vertex_history.h`)
  rebuilt its GPU records when the scope swapped the first-person draws: 4 buffers
  and 6 views a record, and a stream-output geometry shader compiled for each VS no
  live record held. Only the scoped Executioner churns records that hard.
- **Fix:** F1, erased records' buffers go to pools and new records take them; F2,
  the stream-output shaders are cached by VS beyond the records. No key: it only
  removes work. Flight 4 made as many records a window as flight 3 (159 to 604),
  90 to 100 % on pooled buffers after the first window, and compiled 13 shaders in
  the flight against 182 taken from the cache.
- **Earlier:** the Steam flight (`75f33838`, DLSS then Off) found the bursts: 3 to 6
  frames at 40 to 104 ms at the real ADS entry and release, path `none`, DLSS only.
  Flight 3 put them in EDVR's render-thread CPU, in foreground capture.
- **Still open, not a hitch: the refusal.** The foreground contract still refuses H
  for a run of frames at the zoom-in and a few at the zoom-out (flight 4: 10 to 168
  spatial frames and 1 to 7 resets a press window). Those frames take the HDR route's
  spatial recovery, and DLSS restarts with a history reset. B (on main) stops the
  resolver rebuild there; A, the root fix for the refusal, waits on Sean.
- **Cause of the refusal:** with the scope, the two 16:9 cameras (S0's kind 0 and
  kind 3) both zoom to 15.000 deg, with the same pose, near plane and phase. On the
  way in, their projection scales are 0.7 to 2.1 % apart, which is inside the world
  prediction's 10 % (`kFlatDomainWorldScaleTolerance`) but not equal. The depth
  prepass (VS F516BF02 / PS B40B0462) is therefore taken for the world's, and H then
  finds its camera is not the selected world camera. The bare Executioner settles
  at 35 / 30 deg, a scale ratio of 1.18, so it is never refused.
- **Ruled out:** see "Ruled out" below. Flight 4: the staging index-range sets (E3).
  Flight 3: the GPU, and EDVR on the game's other threads. Flight 2: the rebuild (R),
  a DLSS feature re-creation, new-pair shader patching, and the Present split as a
  GPU reading. Earlier: perception (H1), game-side load (H2), S0, the census key,
  camera-observer cost, a reset on any zoom, slower sustained pacing.
- **Instruments, never merged (Sean: "Strip, then merge"):** the ADS frame trace
  (`ads frame trace (S0)`, `src\d3d11\ads_frame_trace.h`), C's Present split and
  verdict reader, and D's census switch (`Census::clockEveryFrame`, `flat_cpu.h`)
  stay on branch `claude/flat-scoped-ads-stutter-3987cc`. That branch was cut from
  S0's (the VR scope magnification arc), so S0 is there too. Main got B, F1, F2 and
  this doc, applied to main as `claude/flat-scoped-ads-fix`. E was never built.
- **D's own cost:** while armed, slow frames of 17 to 31 ms with `e` up to 12 ms
  spread over families (flights 3 and 4). Flight 4's press medians with every frame
  clocked: 11.3 to 12.2 ms.
- **Issues:** none filed yet (waiting on Sean's OK).

## Environment

Epic install, the flat profile of a `both` install. Build `v0.22.0-rc.3-5-gd90db147`
(S0, branch `claude/vr-scope-zoom-design-6ef950`). Log
`edvr_gfx_20261029_052412.log`, 05:24:12 to 05:33:29. Flat AA is DLSS model m, HDR
route active: the scene H is 3840x2160 and the window 1920x1080 (game SS), so DLSS
runs as DLAA at H. Frame cap 90 fps (450-frame windows). Flat read `edvr-flat.ini`
(present, 992 bytes) and nothing else.

## What a flat log carries

- `flat cpu 5s`: frames, the Present interval's p50 and p95, GPU frame p50 and p95,
  GPU resolve p50 and p95, all per 5 s window. CPU timing was off.
- `monitor: long frame counts` measures against the runtime's predicted period. Flat
  has none: the monitor wrote only its session-close line, all zeros, and no
  periodic line, so it never ran.
- **No per-frame frame time.** A hitch of a few frames cannot move a 5 s p95 of
  about 400 frames. The ADS frame trace below adds per-frame timing.

## Evidence: the Executioner presses

S0 flat lines: presses 33 to 35 are scoped (kind 0 goes 56.25 to 15.000 deg, kind 3
goes 47 to 15.000). Presses 36 to 38 are bare (kind 0 to 35.000, kind 3 to 30.000).
The journal flag is the hold's start. "Untreated" is frames minus HDR-route-treated
frames. "Refused" counts frames with the jitter failure reason
`flat-resolve-foreground-contract-unqualified`. `hdr-spatial`, `allocations` and
`req-reset` are the resolver's cumulative counts.

| 5 s to | frames | Present p50/p95 ms | GPU p50/p95 ms | untreated | refused | hdr-spatial | allocations | req-reset | in window |
|---|---|---|---|---|---|---|---|---|---|
| 05:31:19 | 418 | 11.61 / 15.54 | 8.14 / 13.18 | 0 | 0 | 2 | 79 | 42 | hip |
| 05:31:24 | 315 | 13.16 / 19.51 | 10.83 / 14.16 | 56 | 46 (42339-42384) | 48 | 83 | 44 | #33 scoped, entry |
| 05:31:29 | 407 | 11.32 / 14.12 | 8.14 / 13.51 | 12 | 4 | 52 | 83 | 45 | #33 held, release |
| 05:31:34 | 388 | 13.11 / 14.15 | 11.91 / 12.78 | 54 | 46 (43058-43103) | 98 | 85 | 46 | #34 scoped, entry |
| 05:31:39 | 388 | 12.96 / 14.30 | 11.92 / 13.98 | 12 | 4 | 102 | 85 | 47 | #34 release |
| 05:31:44 | 372 | 13.38 / 14.24 | 12.21 / 13.21 | 0 | 0 | 102 | 85 | 47 | hip |
| 05:31:49 | 422 | 11.16 / 14.04 | 7.94 / 12.52 | 62 | 50 (44022-44380) | 152 | 87 | 48 | #35 scoped |
| 05:31:54 | 387 | 12.67 / 14.66 | 11.80 / 14.05 | 4 | 0 | 152 | 87 | 49 | #35 release, #36 bare |
| 05:31:59 | 425 | 11.28 / 14.29 | 10.27 / 12.76 | 0 | 0 | 152 | 87 | 49 | #36, #37 bare |
| 05:32:04 | 400 | 12.99 / 14.26 | 11.81 / 13.94 | 0 | 0 | 152 | 87 | 49 | #37 bare |
| 05:32:09 | 409 | 11.63 / 15.17 | 10.38 / 13.61 | 0 | 0 | 152 | 87 | 49 | #38 bare |

The 32 presses of the eight other weapons (05:26:34 to 05:31:06) left 0 to 5
untreated frames a window, with no foreground-contract refusal. `hdr-spatial` stayed
at 2 for the whole flight before press 33.

`flat foreground pending-null mismatch 1/12` to `12/12` are the session's only
twelve, frames 42339 to 42350 (05:31:23.34 to 05:31:23.47). The witness is
`predicted-world-near`, VS F516BF0201303B87 / PS B40B0462256E31C2, and it differs in
camera only: the same position, near plane 0.025 and phase (0,0). The witness/selected
projection scale ratio runs 1.007 to 1.021 while both y-scales move about 6.70 to
6.99 (about 16.4 to 17.0 deg).

## Mechanism

- In ADS, a pre-naming depth draw whose near plane equals the last world camera's
  is predicted to be the world's when both projection scales are within 10 %
  (`flat_domain_admission.h:234-244`). It is planned World and leaves a witness.
- H requires the witness's camera to match the selected world camera
  (`flat_runtime.cpp:5627-5630`, `foreground-pending-null-not-selected-world`). If it
  does not, the resolve is refused at the foreground contract
  (`flat_mono_resolve.cpp:1085-1087`).
- A refused HDR frame takes the spatial recovery: the jitter is resampled away, with
  no history and no SDK (`flat_runtime.cpp:7535-7550`). DLSS then comes back with a
  reset.
- `allocations` counts the resolver re-creating its render-size textures when its
  resource identity changes (`flat_mono_resolve.cpp:662-669`). It rose 4, 2 and 2 on
  the scoped presses and 0 on the bare ones. Which identity field differs between
  the spatial frame and the DLSS frame is not traced.
- Why only the scope: these are the only cameras flown whose FOVs converge, both at
  15 deg. Once both settle at exactly 15.000, H matches again. Elsewhere they settle
  1.18x (bare) to 2.8x apart.
- Fix direction (unbuilt): when two cameras converge, scale cannot tell them apart,
  so the prediction needs another discriminator. The comment's own numbers show why
  a tolerance cannot: the world moves up to 3.8 % against its own previous frame,
  and these two cameras differ by 0.7 to 2.1 %. In steady state only kind 3 carries
  EDVR's jitter phase. Here, though, the jitter was already warming at phase (0,0)
  from a `no-known-tone-pass` refusal 8 frames before the run (frame 15080 on Steam,
  42331 on Epic), so phase did not separate them either. Whether the
  scope body takes world motion into DLSS once both cameras are equal (a smear on a
  pan) is also unmeasured.

## Hypotheses

1. **Perceptual (H1).** Ruled out by the Steam flight (below).
2. **Game-side load at 15 deg (H2).** Ruled out by the Steam flight (below).
3. **EDVR (H3).** Confirmed. The image change alone (H3a) is not the explanation:
   the frame time does vary, by up to 98 ms frames in bursts of 3 to 4. Those frames
   sit at the refusal run's edges, and only some carry a reallocation, so H3b as
   stated (the reallocation alone) is narrower than what was measured.
4. **S0 (H4).** Ruled out (below).

## Ruled out

- ruled out: contention on S0's camera table, because every camera refresh ran on
  the Present thread (`off-thread=0` in all 110 `flat camera inject 5s` windows).
- ruled out: S0's log I/O as the scoped-only cause, because S0 writes one line a
  press, at release, for every weapon alike (38 lines), and the bare presses show
  none of the scoped signature. S0's code has no scoped branch.
- ruled out: `advanced.vr_camera_census` in flat, because flat read `edvr-flat.ini`
  whole, which has no such key, so the census was off.
- ruled out: camera-observer cost growing with the scope's cameras, because
  `cameras=16` in every window and refresh calls per frame were 142 to 158 scoped
  against 159 to 165 bare.
- ruled out: a resolver reset on any ADS zoom, because the other weapons' zooms (up
  to 2.47x) and the bare Executioner's (1.70x) left no refusal and no reset.
- ruled out: slower sustained pacing in scoped ADS, because scoped-held windows have
  Present p95 14.12 to 14.30 ms, the same as bare (14.26 to 15.17) and the hip.
  This does not cover a hitch of a few frames.
- ruled out: perception (H1), because Sean felt the stutter only with DLSS on, and
  the trace measured 40 to 98 ms frames on DLSS scoped presses only.
- ruled out: a game-side hitch at 15 deg (H2), because with AA Off the scoped presses
  peak at 15.9 to 17.4 ms with 0 or 1 slow frames, while the same cameras converged
  (63 to 127 frames within 10 %).
- ruled out: VRAM pressure behind the bursts, because the card used 7 to 10 GB of a
  31 GB budget during the presses.
- ruled out: the HUD layer's re-arm as the release burst's cause, because it backed
  off and re-armed once in the flight (press 1: 06:12:13.259 and 06:12:15.257), while
  all four scoped presses have a release burst.
- ruled out: an exact camera identity for the prediction, because the world and the
  first-person weapon camera are the same kind-3 object refreshed with different
  FOVs (`flat_camera_vr.h:11-15, 34-43`), and they share one shader pair and one b1.
- ruled out: the resolver's rebuild (R) as the entry burst's cause, because with B
  (flight 2, `d1dad253`) no press has a reallocation, and the scoped entries still
  burst to 45, 25 and 96 ms on the same `none` frames.
- ruled out: a DLSS feature re-creation at the route flips, because flight 2 logged
  one `dlss: the feature ... was created` line, at 09:35:33 (the session's start),
  and none during the presses.
- ruled out: shader patching for new pairs at the zoom, because
  `flat unknown projection capture` held at 13 pairs from 09:37:13 through every
  press of flight 2.
- ruled out: reading GPU-bound frames from the time in the real Present (C's `p`),
  because Elite never waits there: `p` is 0.2 to 0.3 ms on all 50 slow frames flight
  2 listed, temporal and `none` alike. The game paces itself elsewhere, so the GPU
  needs its own clock (D).
- ruled out: the GPU as the burst's cause, because on every burst frame over 40 ms in
  flight 3 EDVR's render-thread CPU (`e`) is 74 to 87 % of the frame, and the GPU span (`g`,
  first game draw to Present) stretches with it while the GPU waits for work.
- ruled out: EDVR's hooks on the game's job threads, because `o` is 0.6 to 1.0 ms on
  every burst frame of flight 3.
- ruled out: the staging index-range sets (E3) as the bursts' cost, because F1 and F2
  leave them as they were, flight 4's scoped windows still requested up to 267 sets
  a window, and the bursts are gone.

## Fix design (2026-10-29, after the Steam flight)

The Steam trace splits each scoped DLSS press into three parts:

1. **Entry burst, 3 frames at 40 to 70 ms.** At the zoom's start the game's post
   chain has no known tone pass for one frame (`no-known-tone-pass`, 8 frames before
   every contract run on Epic and Steam). That frame goes through the copy route's
   spatial fallback, which hands the resolver `hdr=false` and the game's 1920x1080
   output (`flat_runtime.cpp:7377-7381`). The resolver's identity check then tears
   down and rebuilds its whole render-size set (`flat_mono_resolve.cpp:662-669`, R),
   and the HDR route's next frame rebuilds it back (R). Both R marks fall on the
   entry burst in all four presses.
2. **The refusal run, about 46 frames at normal frame time** (frames 15088 to 15096
   ran 12 ms each). It costs the picture (no DLAA, then a reset), not frame time.
   Cause: the prediction.
3. **Release burst, 4 frames at 50 to 98 ms, no R.** These are untreated frames
   (path `none`). Press 1's window shows 14 selection refusals,
   `conflicting-hdr-target-or-camera`. No `flat runtime conflict` line explains them:
   `reportConflict` had no cause. The mechanism is not established.

Components:

- **B, the fallback stops rebuilding the resolver (root, for part 1).** The spatial
  fallback gets its own colour copy and output texture, keyed by its own sizes,
  instead of calling `resources()`, so the temporal set survives a fallback frame.
  Cost: one render-size colour copy (33 to 66 MB at 3840x2160, by format) and one
  output-size texture, made on first use. It covers every fallback, not only the
  scope.
- **A, a second reference for the prediction (root, for part 2).** The first-person
  reference F is the near-equal camera the scale test rejects, or a witness H found
  was not the world. A pre-naming draw is predicted world only when its scale is
  nearer the world reference W than F. It is not exact: it can misread when a camera
  moves more than half the gap between them in one frame (gaps of 0.7 to 2.1 %
  against moves of up to 1.2 % a frame at the end of the zoom on Epic).
  - A first-person draw misread as world still fails H, as today.
  - A world draw misread as first-person would be captured, so it needs a new H
    check: refuse a frame whose captured first-person draws carry the selected world
    camera.
  - The test pins in `tools\flat_temporal_test\flat_domain_admission_tests.h:393-639`
    change.
- **C, instrument the release burst.** On each slow frame the trace adds the HDR
  route's verdict, the jitter refusal reason, and the time spent inside the real
  Present, which separates GPU-bound from CPU-bound frames.

Proposed: build B and C and fly once (DLSS: scoped three times, bare three times).
Expected: no R marks and no entry burst, and C names the release burst. A follows
once that mechanism says whether A removes the release burst too. The one-build
alternative is B, A and C together, with A's H check.

## Side findings (not this arc)

- `flat resolve reset event` lines cap at 32 a session. A storm of 16 resets during
  SMG press 22 (05:29:47, one every third frame, `lost=1 gap=1`, selection
  `relevant-observations-truncated:22`) spent the cap before the Executioner, so
  later resets show only in the cumulative counts.
- The journal's ADS flag lags the zoom. On press 33 the cameras were already at about
  16.4 deg of 15 within 45 ms of the flag. S0's flat ramp times run from the flag.

## The ADS frame trace (instrument)

S0's flat tick (once a Present, on the Present thread) pushes one frame into a ring
of 2048: the Present-to-Present interval, the resolve path from the resolver's
counts (temporal, spatial or none), a reallocation (R), an accepted reset (X), and
the 16:9 kind 0 and kind 3 cameras' vertical FOV. One second after each flat press's
release it writes one line covering 1.5 s before the journal flag to 1.0 s after the
release:

`ads frame trace (S0): press N item X hold H s; F frames a..b s; interval median p95 max ms at +t s (path marks, kind 0 deg); slow (over 1.5x median) K: [+t ms path marks, ...]; paths temporal spatial none (spatial first..last s); reallocations n at ...; resets n at ...; kind 0 first (min), kind 3 first (min); scales within 10% and unequal n (first..last s), equal n`

A once-only `ads frame trace (S0): watching` line says the trace ran. GPU time per
frame is not traced: a GPU stall shows as a long Present interval. The checks are in
`tools\native_frame_test\ads_frame_trace_checks.h`.

## Flight card: Steam, flat, one flight

Install this branch's build to Steam, scope flat. On foot, anywhere the 10-29 flight
flew, with the scoped and the bare Executioner in the loadout. Leave about 10 s
between sets.

1. F8 AA = DLSS. Scoped Executioner: ADS three times, about 3 s each, with a slow pan
   while held. Then the bare Executioner three times, the same way. After each set,
   note whether it stuttered, and where: entry, settle, pan or release.
2. F8 AA = Off (live: the mode is read every frame). Scoped three times, bare three
   times, the same, with the same notes.

Read: `python tools\edvr_log.py --target steam --expect-build HEAD --version`, then
`--grep "ads frame trace \(S0\)"` and `--health`.

| What the flight shows | Verdict |
|---|---|
| DLSS scoped: slow frames on the R or spatial-edge frames; bare and Off clean | H3b: EDVR's rebuild hitches (fix the prediction) |
| DLSS scoped: a spatial run but no slow frames; felt in arm 1 only | H3a: the image change (same fix) |
| Off scoped: stutter felt, slow frames at the zoom ramp (path none, no R) | H2: game side (first press only means a first use) |
| Felt in both arms, no slow frames anywhere | H1: perception |

In arm 2 the trace should read paths `none` with no R or X. Arm 1 scoped should show
about 46 spatial frames and R marks, as the 10-29 log does. If not, the repro
differs and the log says how.

## Flight card 2: B and C, Steam, flat, DLSS

Steam, flat, F8 AA = DLSS. On foot, with the scoped and the bare Executioner. Scoped:
ADS three times, about 3 s each with a slow pan, about 3 s apart so the trace windows
do not overlap. Then the bare one three times. Note whether it stuttered at entry and
at release.

Read: `python tools\edvr_log.py --target steam --expect-build HEAD --version`, then
`--grep "ads frame trace \(S0\)"` (two lines a press: the summary and the slow
frames).

| What the trace shows on scoped presses | Reading |
|---|---|
| no R, and no entry burst | B works: the entry burst was the rebuild |
| an R remains at entry | something other than the spatial fallback flips the resolver's identity; the slow-frames line names the frame's verdict |
| release burst, `p` near the interval | the frame waited in the real Present: GPU-bound (EDVR's GPU work or the driver) |
| release burst, `p` and `h` small | the time went in the game's frame on the CPU (the game or EDVR's draw hooks); the verdict names the route's state |
| no release burst | the release burst went with the entry's rebuild; A is for the picture only |

`A` marks the first frame a fallback makes its own image. It is expected once a
session per kind, not every press.

## Instrument D (designed after flight 2, built on Sean's word)

Three causes are left for a `none` frame of 25 to 96 ms, and the log has no
per-frame figure that separates them:

| Cause | Signature on the slow frame |
|---|---|
| GPU: the frame's GPU work (the game's or EDVR's passes) | GPU time near the interval |
| EDVR's CPU in its hooks (selection, foreground capture, UI layer, the copy route) | EDVR's CPU near the interval, and the family that took it |
| The game's or the driver's CPU outside EDVR's scopes | both small |

- **D1, GPU time per frame.** The CPU census already times every frame on the GPU,
  first game draw to Present (`gpuFrameOpen`/`gpuPoll`, `flat_runtime.cpp`), but
  only into the 5 s percentiles. Each timer slot now keeps the frame number it opened
  on (`gpuFrameOf`); when its reading comes back (a few frames later, never waited
  for), `flatRuntimeTraceGpu` hands it to the trace, which puts it on that frame.
- **D2, EDVR's CPU per frame.** The census (`flat_cpu.h`) clocks one frame in 16 at
  random, so it has not caught a burst frame. `adstrace::Arm` arms while the journal's
  ADS flag is up, or kind 0's FOV moved by more than 0.05 deg since the last frame,
  and for 1.5 s after; while armed, `Census::clockEveryFrame` clocks every frame on
  every thread. `Census::lastFrame` gives the cut frame's EDVR render-thread total
  (the line's families plus engine motion's folded hooks), its heaviest family (copy
  checks with their children), and the other threads' total. It needs CPU timing on
  (F8 > Debug), which Sean's Steam flat has.
- **The line:** a third line a press, `press N slow frame costs`, gives each slow
  frame's `g` (GPU), `e` (EDVR's render-thread CPU) with its heaviest family, and `o`
  (EDVR on the game's other threads), in ms, `none` when not measured. Its header
  counts the window's armed frames and the frames with a CPU and a GPU figure.
- **Cost:** a clocked frame costs 1.7 ms on foot (2026-09-30) to 7 to 10 ms in a
  settlement (`flat_cpu.h`, 2026-10-16), and only frames around a zoom pay it. The
  burst frames are 25 to 96 ms, so they still stand out. While it runs, the
  `flat cpu 5s` figures over-weight zoom frames. Both go with the trace when the
  arc closes.
## Flight card 3: D, Steam, flat, DLSS

Flight card 2 again: Steam, flat, F8 AA = DLSS, F8 > Debug > CPU timing in the log
on. Scoped Executioner: ADS three times, about 3 s each with a slow pan, about 3 s
apart. Then the bare one three times. Note where it stuttered.

Read: `python tools\edvr_log.py --target steam --expect-build HEAD --version`, then
`--grep "ads frame trace \(S0\)"` (three lines a press). On the scoped presses'
`none` burst frames:

| What the costs line shows | Reading |
|---|---|
| `g` near the interval | the GPU took the frame: next, the EDVR GPU census's passes for that window, and whether the game's or EDVR's |
| `e` near the interval | EDVR's CPU on the render thread: the family named is the place to look |
| `o` large | EDVR's hooks on the game's job threads, which the render thread waits on |
| `g`, `e` and `o` all small | the game's or the driver's CPU outside EDVR's scopes (with DLSS only: something EDVR's state makes the game or driver do) |
| `e none` on the burst frames | not clocked: check the header's armed count and that CPU timing was on |

## Foreground capture at a zoom edge (code read after flight 3)

Nothing under the two `kForegroundCapture` scopes opens a scope of its own, so all
of `motion.capture` and `engineVelocityFlatDomainBeginDraw` counts there. Blocking
readbacks are ruled out by reading: every production Map in the foreground is
`DO_NOT_WAIT`, and the waiting paths are called only from `tools\weapon_motion_test`.

| Candidate | Where | When it runs |
|---|---|---|
| E1. record creation: 4 `CreateBuffer` (2 of `count*16` B, SO and SRV) and 6 views, no pooling | `allocateBuffers`, `animated_vertex_history.h:610-631` | every new draw key; a key change (`new-key`, `buffer-change`, `count-change`) reuses nothing, and at the 128-record cap each new record first erases a spent one |
| E2. `CreateGeometryShaderWithStreamOutput` (a driver compile of the original VS) | `animated_vertex_history.h:612-619` | a record whose VS no live record shares: kept only through live records, so a VS unseen for over 2 frames compiles again |
| E3. staging index-range sets, up to 512 KB each | `ensureVertexSet`, about `:796-826` | a record with no cached set |

The 5 s windows of flight 3 fit E1 or E2: windows with scoped presses made 158 to 460
records (peak at the 128 cap, 48 to 211 reclaimed under pressure); bare-press
windows made 13 to 41 (peak 47 to 49). Per frame is not measured.

**Instrument E (proposed, not built):** per frame, the records created, the geometry
shaders compiled and the staging sets made, each with its ticks, added to the costs
line.

**The fixes (built on Sean's word without E; `animated_vertex_history.h`):**

- **F1, pools.** Both erase paths (`advance` and the pressure reclaim) hand the
  record's buffers to `poolBuffers` before the erase. The identity set (2 buffers,
  2 SRVs, 2 UAVs, 16 B each) fits any record. The position pair fits a record of
  `count` vertices when its capacity is `count` to `2*count`, smallest first; its
  views come too when they cover exactly `count`, else 2 views of `count` are made
  over it. Pools hold at most 128 sets, and the position pool at most 32 MB.
- **Why reuse is safe:** an erased record was last used 2 or more frames before. Its
  buffers are not read before the new record's own capture writes them (a prior is
  a record published the frame before), the same as a new buffer's undefined bytes.
- **F2, shader cache.** `streamOutputs_` holds up to 64 stream-output shaders by
  original VS, least recently used out. Each entry holds its VS, so the key's address
  cannot be reused while cached.
- **Counters:** `flat foreground history 5s` ends with `records made with: reused
  positions= reused identity= stream-output shaders compiled= cached=`.
- **Checks:** `tools\weapon_motion_test\flat_bench_history_pressure_tests.h` "zoom
  edge" (WARP): the cached shader and pooled sets are taken after every record of
  the VS is gone, the reused buffers capture the bytes a fresh history does, a
  larger pooled pair gets views of the record's own count, and no pair over twice the
  count is taken. `flat_range_invalidate_tests.h` pins that both erase paths pool.

## Flight card 4: F1 and F2, Steam, flat, DLSS

Flight card 3 again (D stays, CPU timing on): scoped Executioner three times, then
the bare one three times. Note where it hitched.

| What the log shows | Reading |
|---|---|
| burst frames gone, or foreground capture a few ms on them | fixed |
| bursts stay; the history line shows `compiled` near 0 and `reused` near `allocations` | the cost is elsewhere in the family: build E (the staging sets, the views) |
| bursts stay; `reused` low | the pools miss (sizes do not fit, or the cap churns): log the sizes |

Flown 10-29: the first row. See the journal.

## Journal

- 2026-10-29: Read the Epic flat log (d90db147). Found the foreground-contract
  refusal, the spatial run, the resets and the reallocations on scoped presses only.
  Ruled out S0 and the census key. Built the ADS frame trace (full build green:
  native_frame_test 2407 checks, 0 failures). Sean: test on the Steam install.
  Committed `75f33838`, `--dll-only` green, installed to Steam as scope both (Steam
  is a both install, so a flat-only install is refused); it replaced main's
  `130e88f1`, which this branch contains.
- 2026-10-29, Steam flight (`edvr_gfx_20261029_060959.log`, build matches
  `75f33838`, DLSS model k). The trace, by press (times from the journal flag; the
  flag lags the real entry and release by about 0.2 to 0.8 s):

  | press | AA | weapon | max ms (at) | slow | paths T/S/N | R | X |
  |---|---|---|---|---|---|---|---|
  | 1 | DLSS | scoped | 93.1 (+3.02) | 39 | 319/46/22 | 3 | 3 |
  | 2 | DLSS | scoped | 98.4 (+4.37) | 57 | 439/45/27 | 2 | 3 |
  | 3 | DLSS | scoped | 79.1 (+4.37) | 43 | 429/92/28 | 4 | 4 |
  | 4 | DLSS | scoped | 85.7 (+2.93) | 39 | 338/49/16 | 2 | 2 |
  | 5 | DLSS | bare | 27.2 (+0.94) | 10 | 397/0/0 | 0 | 0 |
  | 6 | DLSS | bare | 21.6 (-1.10) | 2 | 400/0/0 | 0 | 0 |
  | 8 to 10 | Off | bare | 28.9, 16.5, 15.7 | 2, 0, 0 | 0/0/all | 0 | 0 |
  | 11 to 13 | Off | scoped | 17.4, 16.1, 15.9 | 1, 0, 0 | 0/0/all | 0 | 0 |

  Press 7 was bare, with the F8 switch inside its window. Press 1's release burst:
  50.0, 83.8, 92.8 and 93.1 ms at +2.73 to +3.02, all `none`. Then
  `flat resolve reset event ... delta-ms=531` (no DLSS for 531 ms) at the release,
  and `flat jitter: state=warming` in that window. Entry bursts run 40 to 93 ms, the
  first frame marked R. Presses 1 to 4 came 1 to 2 s apart, so their windows overlap
  and a burst can be listed twice: press 2's -1.3 s frames are press 1's release, and
  press 1's +3.95 s frames are press 2's entry. Each max in the table is the press's
  own. Sean: stutter with DLSS on only.
- 2026-10-29: Built B and C (Sean: "Build B + C").
  - B: `flatMonoResolveSpatialFallback` no longer calls `resources()`. `spatialImages`
    lends the temporal set's input copy (and the copy route's output) when they fit
    the frame, else the recovery's own images, one per format, made once
    (`stats.spatialAllocations`).
  - Checks: `flat_mono_resolve_test` pins an unfit fallback (two own images, no
    rebuild then or at the next resolve, reuse on a second); `flat_hdr_crumbs_tests.h`
    pins that the recovery never asks `resources()`.
  - C: `presentClockLast` (`device_hook.h`) and `flatRuntimeTraceRead`
    (`flat_runtime_trace.h`) feed the trace's second line.
  - Full build green (native_frame_test 2410 checks, 0 failures).
- 2026-10-29, Steam flight 2 (`edvr_gfx_20261029_093511.log`, build matches
  `d1dad253`, DLSS, CPU timing on). Sean: "it stutters on entry".

  | press | weapon | max ms (at) | slow | paths T/S/N | R | X | entry burst ms | release burst ms |
  |---|---|---|---|---|---|---|---|---|
  | 1 | scoped | 45.4 (-0.28) | 10 | 531/52/12 | 0 | 2 | 45.4, 36.2, 28.3 | 20.4 |
  | 2 | scoped | 26.0 (+7.01) | 17 | 590/53/12 | 0 | 2 | 25.0, 22.9, 22.2 | no `none` frame listed |
  | 3 | scoped | 96.0 (-0.60) | 55 | 483/39/20 | 0 | 2 | 96.0, 80.8, 76.6 | 31.5 to 94.4 (6 frames) |
  | 4 to 6 | bare | 32.2, 20.4, 27.3 | 5, 5, 17 | all T | 0 | 0 | - | - |

  - Every burst frame is path `none`, verdict `conflicting-hdr-target-or-camera`;
    the first of each entry has the jitter `warming`. No listed frame carries A.
  - `p` 0.2 to 0.3 ms and `h` 0.0 ms on all 50 listed frames.
  - Press 3's entry: the UI layer's `tone candidate` lines (route `trained-native`,
    meaning the HDR route had not treated the frame) for frames 14688 to 14690 are 81
    and 77 ms apart. 14688 is also the jitter census's first `no-known-tone-pass`
    frame and the foreground's `new-key` example: the scope's first frame.
  - No `flat runtime conflict` line in the flight: the selector's conflict carried no
    witness cause.
  - Conclusion: B works as built (no R anywhere) but was not the burst's cause.
    Designed D (above). Not built: waiting on Sean.
- 2026-10-29: Built D (Sean: "Build D").
  - Checks: `flat_cpu_tests.h` pins `clockEveryFrame` (every thread on every frame
    from the call, the schedule again after, nothing with the clocks off) and
    `lastFrame`'s figures; `ads_frame_trace_checks.h` pins `Arm`, `Ring::find` and the
    costs line; the census-touch pin in `flat_temporal_test.cpp` now counts 15, with
    `lastFrame` read only in `flatRuntimeTraceRead`.
  - The first full build failed on that pin (13 counted); fixed.
  - Full build green (native_frame_test 2423 checks, 0 failures).
  - Committed `e5741032`, `--dll-only` green, installed to Steam (scope both, XeSS),
    verified.
- 2026-10-29, Steam flight 3 (`edvr_gfx_20261029_104722.log`, build matches
  `e5741032`, DLSS, CPU timing on). Sean: "it hitches in a similar spot on both the
  zoom in with ADS and the zoom out after release".

  | press | weapon | max ms | paths T/S/N | entry burst: ms (e, foreground capture) | release burst: ms (e, foreground capture) |
  |---|---|---|---|---|---|
  | 1 | scoped | 78.0 | 432/48/12 | 78.0 (66.7, 51.8), 58.3 (46.6, 36.9), 45.0 (34.4, 26.9) | none listed |
  | 2 | scoped | 75.1 | 412/49/16 | 71.1 (60.2, 48.7), 51.2 (39.6, 31.1), 43.4 (32.1, 24.8) | 56.8 (46.3, 36.5), 63.7 (52.0, 41.8), 75.1 (63.7, 51.2) |
  | 3 | scoped | 103.5 | 363/47/16 | 77.0 (66.8, 54.1), 62.5 (52.0, 41.3), 57.0 (46.3, 36.7) | 84.3 (71.5, 58.0), 98.1 (82.8, 67.1), 103.5 (89.7, 73.2) |
  | 4 to 6 | bare | 26.3, 26.3, 31.2 | all T | - | - |

  - Every frame in the table is path `none`, verdict
    `conflicting-hdr-target-or-camera`; `g` is 3 to 6 ms under the interval on each,
    and `o` 0.6 to 1.0 ms. The bursts' smaller edge frames (24 to 29 ms) have `e` 11
    to 16 ms, foreground capture 3 to 12 ms.
  - Other slow frames (20 to 31 ms, temporal, scoped and bare) have `e` 7 to 12 ms
    with `other` or `contract reduction` heaviest at 1 to 2 ms: D's clocking.
  - Per 5 s window, foreground capture averages 0.5 to 1.5 ms over 70 to 326 calls
    per clocked frame; 0 before the first press.
  - Conclusion: located in EDVR's foreground capture, render thread.
  - `flat foreground history 5s`: scoped-press windows made 158 to 460 records (peak
    128, pressure-spent 48 to 211); bare-press windows 13 to 41 (peak 47 to 49).
  - A code read (subagent) named E1 to E3 above; E1 and E2 checked by hand at
    `animated_vertex_history.h:199-243, 604-631`.
- 2026-10-29: Built F1 and F2 (Sean: "Fix both now"), described above.
  - The first full build failed on two pins: the stats struct's field count (now 28)
    and a second `from-cache=` in the history line (renamed `cached=`). Fixed.
  - Full build green (native_frame_test 2423 checks; weapon motion 229691 checks on
    WARP, the zoom-edge checks among them).
  - Committed `97193c14`, `--dll-only` green, installed to Steam (scope both, XeSS),
    verified.
- 2026-10-29, Steam flight 4 (`edvr_gfx_20261029_122815.log`, build matches
  `97193c14`, DLSS, CPU timing on). Sean: "The hitches are gone!"

  | press | hold s | max ms | median | slow frames | paths T/S/N | resets | heaviest `e` on a slow frame, ms |
  |---|---|---|---|---|---|---|---|
  | 1 | 0.99 | 28.9 | 12.15 | 12 | 163/86/24 | 4 | 11.1 (discovery 2.7) |
  | 2 | 1.10 | 23.3 | 11.56 | 3 | 156/114/28 | 5 | 6.0 (copy checks 1.4) |
  | 3 | 0.98 | 23.3 | 11.44 | 1 | 216/57/16 | 3 | 6.0 (copy checks 1.4) |
  | 4 | 0.98 | 24.5 | 11.61 | 8 | 266/10/8 | 1 | 8.4 (foreground capture 2.3) |
  | 5 | 2.96 | 24.5 | 11.34 | 13 | 254/168/32 | 7 | 8.4 (foreground capture 2.3) |
  | 6 | 8.10 | 27.7 | 11.34 | 51 | 723/116/32 | 7 | 12.4 (copy checks 3.1) |

  - All six are the scoped Executioner (kind 0 reaches 15.0 to 15.2 deg on each); no
    bare presses this flight. Windows of presses under 3.5 s apart overlap, so a
    frame can be listed twice (presses 2 and 3, 4 and 5).
  - No frame over 29 ms; flight 3's scoped bursts were 43 to 104 ms. The two slowest,
    28.9 and 27.6 ms, are 1.2 to 1.3 s before press 1's ADS flag, not clocked
    (nothing armed yet), GPU 11.0 and 13.7 ms: not attributed.
  - Foreground capture is the heaviest family on one slow frame, at 2.3 ms; on the
    rest it is copy checks, discovery or other, 0.3 to 3.1 ms. `o` 0.5 to 1.5 ms.
  - Press 6's release (+8.0 to +9.1 s) holds its slowest frames, 22 to 28 ms, with
    `e` 8 to 12 ms spread over families: the same class as flight 3's ordinary slow
    frames (20 to 31 ms, `e` 7 to 12 ms), D's clocking.
  - `flat foreground history 5s`, the six windows with the scope out (12:30:12 to
    12:30:37): 235, 604, 422, 308, 353 and 159 records made; positions reused 87,
    544, 420, 297, 352, 159 and identity sets 107, 604, 422, 308, 353, 159.
    Stream-output shaders compiled 7 and 6 in the first two windows and none after;
    182 taken from the cache in the flight. Pressure-spent 4 in the first window and
    0 after (flight 3: 48 to 211). The scope still swaps the draws as often; each new
    record is now cheap.
  - Conclusion: F1 and F2 fixed the stutter. Ruled out E3.
- 2026-10-29: Sean: "Strip, then merge". The arc's branch was cut from S0's, so
  merging it would have brought S0 and the VR scope design doc to main. Instead, B
  (`d1dad253`'s `flat_mono_resolve` and test hunks), F1 and F2 (`97193c14`'s source
  and tests) and this doc were applied to main `562759c0` as
  `claude/flat-scoped-ads-fix`; both applied cleanly (main had not touched those
  files). The ADS frame trace, C and D are not on main. Full build green (all gates;
  495 s).
  - A review subagent read the diff. No bug it could make fire; two changes taken:
  - A record invalidated this frame is no longer pooled (`poolBuffers` returns).
    A capture revalidates its record, so one invalidated now may have been captured
    earlier this frame. If a deferred invalidation (`adoptSet`) left its draw in the
    flat adapter, a record made later in the frame could have written the buffers
    that draw's H pass reads. Flight 4 had `pressure-invalidated=0` in every
    window, so the zoom edges lose no reuse. `advance` runs after the adapter clears
    its draws, so that path is unchanged.
  - A shader taken from a live or reclaimed record now refreshes its cache entry.
    Before, a VS served only by live records kept its first `lastUsed`, so it was
    the first evicted once 64 others had been cached.
  - Noted, not changed: position pairs up to twice a record's count are counted at
    the record's count in `bytes_`, and the pools hold up to 32 MB of idle buffers
    until the history resets; `allocations=` still counts records made from pooled
    buffers.
  - Checks: the snapshot-lifetime case in `flat_bench_history_pressure_tests.h` now
    shows no pooling for the same-frame reclaim and pooling at the next boundary;
    `flat_range_invalidate_tests.h` pins both changes. Unflown: both only remove
    reuse in a rare path or keep a cache entry longer.
