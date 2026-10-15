## Status

State: BUILT, NOT FLOWN. Branch claude/gpu-profile-v2 (cut from origin/main 3fd97a97). A measurement only: no draw, binding or
shader changes. Ported from claude/gpu-pass-profile (0cdf493c), the yield of claude/hangar-test-9 (101708de) and its v13.7
classes and hashes (9a8e40e1). The sealed-trial code is not ported.
Key: hotkey.gpu_pass_profile (TEMPORARY, default unbound). Value is "KEY" or "KEY COUNT": COUNT is the frames per press,
1 to 120, default 30. Example: "F9 60". Flat allow-list: src\common\flat_profile_keys.h. Commented in edvr.ini.
Question: where Elite's own GPU time goes each frame. Sean sees about 9 ms GPU in fpsVR in a station hangar whatever the view
direction. EDVR's census prices its own share (about 5.5 ms) and estimates the game's (about 5 ms); neither splits Elite's frame.

Open hypotheses (each has the signature that confirms or refutes it in the CSV):
- H1 probes: reflection-probe cube faces re-rendered on turns. Confirm: probe_cube rows, a run of six same-size square passes,
  ms per frame large and stable across view directions. Refute: probe_cube under about 0.3 ms.
- H2 shadows: sun and light shadow maps. Confirm: shadow rows; count per frame (1 means once per frame, 2 means per eye).
- H3 per-eye duplication: scene_gbuffer and forward_at_eye rows come in pairs. Confirm: pass count per frame even, halves match
  in ms. Refute: a single eye-sized set per frame.
- H4 full-screen passes at eye size: fullscreen_at_eye total ms. Confirm if more than about 2 ms a frame.
- H5 post chains: post_smaller total ms and the sizes in its signatures.
- H6 lighting: lighting_volume rows (instanced light volumes into the eye-size target, colours=2). From
  reviews\sealed-remaining-costs-2026-10-13.md: 0.63 ms a pass in the sealed hangar, sun draw inside it. Confirm: the
  lighting_volume median and the shader column naming 0357BBB2/8289669D/24DE25E4/A1B7CFCD. Refute: no lighting_volume
  rows in the sealed hangar (the classes are wrong) or its total well under the review's 0.63 ms a pass.
- H7 compute: top_cs_dispatches (all dispatches). Confirm: a named CS (APPLY_BIND_POSE_TRANSFORMS_CS or CLEAR_TRANSFORM_DATA_CS)
  with a large per-frame ms in the compute rows. A 0000000000000000 entry means the registry never met that shader.
Ruled out: none yet.

Changes in v2 (2026-10-13):
- CS hash: bindingShaderHash(Cs) was always 0 (CSSetShader records the pointer, never the hash). The profiler now looks the
  hash up at Dispatch, only while a capture runs (lookupShaderHash, exposure_fix.cpp). No cost on compute binds otherwise.
- Count: hotkey value COUNT (1..120, default 30). The timestamp pool is sized to the count, created at the first capture of
  that size (log line gives its time: a one-time hitch), and replaced if a later capture asks for more.
- Summary: means and medians over the captured frames. The top list is the top 20 passes by median, each with its shader
  identity (VS:PS, or cs:hash(NAME) for dispatch-only passes), render-target size and draw count. top_cs_dispatches lists the
  top 5 compute shaders by all dispatches per frame.
- Memory: each frame's segments are freed once its rows are built (a 120-frame capture holds rows, not segments).

v3 fixes (2026-10-13, after the six flights on cb712758; root causes in the Log):
- Only 4 of 30 frames captured: 4 pass-scope slots for 30 frames. Now 128, and a boundary waits for another holder of the clock.
- probe_cube on eye passes: squareness is not the rule any more. probe_cube = not at the eye size, and square up to 1024 or a cube array.
- v4: 252x252 one-draw passes and the dispatches whose top compute shader is B8E727E9 or F1FB2EEF are "exposure" (Elite's exposure chain; reviews\vr-view-caches-2026-10-13.md). probe_cube's square is now 256 to 1024 (or a cube array of 6+ slices).
Delay (2026-10-14, branch claude/gpu-profile-delay, TEMPORARY): hotkey value KEY COUNT DELAY_MS (delay 2 to 12 ms, default 6). Frame 1 of a capture is the reference (no delay); the others carry a compute delay at their start, as their own segment, excluded from classes and totals. Summary: "delay D ms (measured M ms) placed at frame start; inter-pass gaps median G ms per frame; frame busy B ms". Suspect passes are marked in the CSV (gap_after_ms, suspect).
UI instruments (2026-10-13, TEMPORARY for this measurement build: g_hdrDrawTimingOn was on in ui_layer.cpp; OFF since 2026-10-14, turn it on only for a measurement flight):
hologram resolve split into depth copy, near-light and resolve draw; moved HDR HUD draw as its own altered item; seeds by reason;
reissues issued vs empty. Lines in the Log.

Flight plan (one press each; the hangar trial is off for all three):
1. Sealed hangar, seated: press the key once (default 30 frames), wait for the summary lines.
2. Sealed hangar, seated, with Optimized station off: press once. Compare lighting_volume and the top passes with step 1.
3. Supercruise: press once. Compare hud_overlay and the top passes with step 1.
A press during a capture is refused with a note. A capture arms at the next frame boundary, not mid-frame.
Check the first line of the log: the version line must name the built commit (edvr_log.py --expect-build HEAD).

Build gates: tools\gpu_pass_profile_test --self-test (in build.bat). The DLL build compiles src\d3d11\gpu_pass_profile.cpp.
Still open: no flight yet; the overhead below is an estimate, not a measurement.

## Log

- 2026-10-13, claude/gpu-profile-v2: ported from 0cdf493c, 101708de and 9a8e40e1 onto origin/main. Changes as in the Status
  block. Rig tests added for the lighting class, shader identity, CS totals, the median top list, pool sizing and the count
  parse. The yield (v13.6) is unchanged: it yields whenever the profiler holds the clock. The app GPU timing default: main's
  code reads advanced.app_gpu_timing with default on (getBool(..., true)). claude/aa-off-tax changes that default to off and
  is not on main. With it off there are no application-frame leases to yield, so the yield is a no-op there.
- 2026-10-13, v3 (the fixes after the six flights of cb712758, both reproduced from the logs):
  1. Frames 1 to 4 captured, then "frame 5 not captured: the shared GPU timing scope was already open". Root cause:
     gpu_timing.h kPassScopeSlots = 4, and the profiler opens frame k in slot k (gpu_pass_profile.cpp, beginFrame). Frame 5 is
     slot 4, which gpuTimingPassScopeOpen refuses (gpu_timing.cpp, the slot check), and the batch ended on that refusal. The text
     named the census and the application frame, which were not the cause. Both already refuse new leases while the yield is on:
     GpuTimer::beginImpl (gpu_timing.cpp:240) and GpuTimingFrameDriver::begin (gpu_timing.cpp:413). The application frame open at
     the press is ended by gpuFrameYieldBegin (gpu_frame_timing.cpp:272) before the first boundary's decision. Nothing else opens a
     standalone scope: acquireInterval is called from GpuTimer::beginImpl (gpu_timing.cpp:256) and the profiler (gpu_pass_profile.cpp).
     Fix: kPassScopeSlots = 128 (static_assert against kMaxCaptureFrames in gpu_pass_profile.cpp). A boundary that finds another
     scope holding the clock waits (startAction) for it to end, up to kMaxStartWaits = 240 boundaries, then the batch ends with a note.
     A faulted frame is skipped and the batch goes on. The rig simulates census and application holders (testCaptureStarts).
  2. Six eye passes in a row (2016x2016, square, single target) were relabelled probe_cube by markCubeRuns, which took any square
     single-target pass. Fix: probe_cube needs a target not at the eye size that is square and at most 1024 on a side, or a cube
     array (6+ slices). The eye size is vscreen's eye texture. The summary logs "eye WxH (the headset's eye texture); the largest
     colour target with a matching depth WxH" so a mismatch shows in a flight.
  3. Offline re-classification (scratchpad reclassify_v3.py): the port reproduces all 5,848 v2 rows first, then applies the v3
     rules. Per capture, ms per frame (4 frames), sum over rows = frame ms:

     | class | planetary port, station on | planetary, off | supercruise | station, descending | station, on | station, off |
     |---|---|---|---|---|---|---|
     | edvr | 5.039 | 5.076 | 4.949 | 5.592 | 5.823 | 5.266 |
     | compute | 1.808 | 1.129 | 0.934 | 1.826 | 1.403 | 1.405 |
     | hud_overlay | 0.746 | 0.615 | 0.668 | 1.722 | 1.249 | 1.218 |
     | fullscreen_at_eye | 0.843 | 0.977 | 1.485 | 0.843 | 0.558 | 0.594 |
     | scene_gbuffer | 0.939 | 1.051 | 0.686 | 0.933 | 1.117 | 1.424 |
     | probe_cube | 1.161 | 0.037 | 0.043 | 0.120 | 0.109 | 0.189 |
     | clear | 0.235 | 0.272 | 1.111 | 0.350 | 0.275 | 0.220 |
     | forward_at_eye | 0.261 | 0.265 | 0.356 | 0.683 | 0.342 | 0.999 |
     | post_smaller | 0.035 | 0.753 | 0.365 | 0.365 | 0.008 | 0.028 |
     | lighting_volume | 0.606 | 0.486 | 0.112 | 0.673 | 0.125 | 0.000 |
     | other | 0.028 | 0.140 | 0.238 | 0.034 | 0.028 | 0.029 |
     | shadow | 0.124 | 0.153 | 0.027 | 0.064 | 0.034 | 0.037 |
     | total | 11.824 | 10.953 | 10.972 | 13.204 | 11.070 | 11.409 |

     Probe_cube in the first capture is 1.161 ms, of which 24 one-draw 252x252 passes are the bulk (the rule the spec gives counts
     them as probes; see Status). Only 4 frames per capture: these are not 30-frame figures.

- 2026-10-13, the UI instruments (from reviews\ui-pass-costs-2026-10-13.md, section 5; census and UI layer lines, no new keys):
  1. The hologram resolve has its three parts on their own census line: "hologram depth copy"
    (ui_depth_layer.h acquire's CopyResource), "hologram near-light" (the near-light Dispatch), "hologram resolve draw" (the resolve draw).
    They print on their own line ("the hologram resolve's parts"), not on the main line, which is at the log's length; they nest inside the umbrella and are never added to a total. Door sections DoorHoloDepthCopy, DoorHoloNearLight, DoorHoloResolveDraw.
  2. The moved HDR HUD draw (the route's kHdrMovedDraw timer) is its own item on the altered line: "HUD draws moved into the HDR
    HUD layer (the game's shading at the layer size)", section AlteredHdrHud, opened with the route timer and closed with it.
    Not EDVR's cost; not in the EDVR total.
  3. g_hdrDrawTimingOn = true (ui_layer.cpp), TEMPORARY for this build: it arms the HDR draw price, the seed census and the seed
    sub-price, as before. Turn it back to false after the flight.
  4. Reissues: the UI layer reissues item now reads "[N real a frame, M empty]". A scope counts as real when a multiply, write-back or
    tonemap re-issue actually issued its draw (GpuCensusScope::markIssued); an empty scope returned before issuing anything.
  5. Seeds by reason, on a new line after "ui quality: layer:": "ui quality: layer seeds: X a frame issued (Y the second or a later
    seed of a layer in its frame), Z planned passes a seed; by reason, a frame, all seeds: first observed a, new frame b, source
    changed c, size changed d, stale draw e, stale clear f, missing depth g, missing stencil h, unknown stale i; by reason, a frame,
    second or later seeds: ...". Bits from UiSeedReason (ui_layer_seed_census.h); a seed with two reasons counts under both.
  Tests: gpu_census_test (instrumentCases: the sub-section order, the door count, the altered class, the issued counter) and
  ui_quality_test (testSeedReasonTally).

- 2026-10-14, delay (branch claude/gpu-profile-delay, from origin/claude/flight-2026-10-14). The flight had GPU slack at 90 Hz, so a
  pass's timestamps include the GPU waiting for the CPU to submit (the AA-off capture put 2.5 ms on cs:b09aadab, a tiny copy).
  The feature: a front-loaded compute delay at the start of each captured frame after the first. Frame 1 is the reference (no
  delay). Calibration, once per capture at the batch start: one blocking trial of 1,000,000 dependent multiply-adds (grown x4, up
  to three tries, until 0.5 ms is measurable), timed by a disjoint query and two timestamps; ms per iteration = trial ms / trial
  count; iterations = delay / ms per iteration. The shader (src\d3d11\gpu_pass_delay_shader.h) is one thread into a one-element
  UAV, compiled at the first capture with d3dcompiler_47.dll and loaded once. DEVIATION: house practice precompiles shaders in the
  build (temporal_shader_build), and a precompiled entry needs its contract table extended; this one compiles at run time. A
  compile failure turns the delay off for the session, with a note. The delay's CS, CB0 and UAV0 bindings are read and restored
  through the context's own methods, and its dispatch is not a pass note (g_delayIssuing).
  Measured: the delay segment's own time (M). Gap: the no-work boundary segments, per frame (median over the delayed frames; the
  reference frame's own line). Busy: the frame time without the delay (work plus gaps; each gap stays folded into the pass after it,
  as before). Suspect: over 3x its key's median and followed by a gap over 0.25 ms (CSV gap_after_ms and suspect; the summary
  counts them). LIMIT: a submission stall sits before the first command of a segment, so it is inside that pass's time, not in an
  empty segment. The gap median can read near zero while the passes are long. The test of whether the delay removed waits is the
  per-pass median and the frame busy time against the reference frame, not the gap alone. Rig: testV5Delay.
- 2026-10-13, v4 (after the coordinator's correction): the 252x252 passes are Elite's exposure chain, not probes (candidate 1 of
  reviews\vr-view-caches-2026-10-13.md: three 252x252 post passes per eye, with compute shaders B8E727E94D44D891 and
  F1FB2EEFB662F3AA). New class "exposure": 252x252 single-target passes, and dispatch-only passes whose top compute shader is one
  of those two. probe_cube's square now starts at 256 (kProbeMinSide). Rig: testV4Exposure. Offline table (ms a frame, 4 frames,
  eye 2016x2016; the v2 mode still reproduces the CSVs' classes, 0 mismatches):

  | class | planetary port, station on | planetary, off | supercruise | station, descending | station, on | station, off |
  |---|---|---|---|---|---|---|
  | edvr | 5.039 | 5.076 | 4.949 | 5.592 | 5.823 | 5.266 |
  | hud_overlay | 0.746 | 0.615 | 0.668 | 1.722 | 1.249 | 1.218 |
  | compute | 1.522 | 0.831 | 0.612 | 1.532 | 1.115 | 1.062 |
  | fullscreen_at_eye | 0.843 | 0.977 | 1.485 | 0.843 | 0.558 | 0.594 |
  | scene_gbuffer | 0.939 | 1.051 | 0.686 | 0.933 | 1.117 | 1.424 |
  | post_smaller | 1.150 | 0.753 | 0.365 | 0.365 | 0.008 | 0.028 |
  | clear | 0.235 | 0.272 | 1.111 | 0.350 | 0.275 | 0.220 |
  | forward_at_eye | 0.261 | 0.265 | 0.356 | 0.683 | 0.342 | 0.999 |
  | lighting_volume | 0.606 | 0.486 | 0.112 | 0.673 | 0.125 | 0.000 |
  | exposure | 0.309 | 0.321 | 0.338 | 0.314 | 0.311 | 0.365 |
  | other | 0.028 | 0.140 | 0.238 | 0.034 | 0.028 | 0.029 |
  | probe_cube | 0.022 | 0.013 | 0.026 | 0.101 | 0.085 | 0.167 |
  | shadow | 0.124 | 0.153 | 0.027 | 0.064 | 0.034 | 0.037 |
  | total | 11.824 | 10.953 | 10.972 | 13.204 | 11.070 | 11.409 |

  Two things in the table are not what they look like. (a) post_smaller's 1.150 in the first capture is one 2x2 cube-face pass
  (frame 0: 3.244 ms, frame 2: 1.159 ms), which the 256 bound moved out of probe_cube. A 2x2 target cannot cost 3 ms, so those
  milliseconds are most likely boundary-less GPU work between two stamps, not the pass. (b) Cube faces of 16 to 128 px (one draw,
  cube resource) are now post_smaller under the spec's 256 bound; their own time is small (about 0.01 ms a frame together).
## What it does

A press arms a batch of COUNT whole frames (the hotkey's value; default 30, capped at 120). At each frame boundary
(vScreenFrameBoundary, once per Present) the frame in progress is closed and the next one opened:

- Each frame has its own frequency scope, opened with gpu_timing.cpp's gpuTimingPassScopeOpen (a standalone scope on the
  shared disjoint clock). It refuses while any other scope is open, so the profiler never borrows a scope it did not open.
- A TIMESTAMP query is issued on the owner context at every pass boundary:
  - an OMSetRenderTargets or OMSetRtvAndUav call (the bound target set changes);
  - ClearRenderTargetView and ClearDepthStencilView;
  - a Dispatch (exposure_fix.cpp's hook);
  - the begin and end of each EDVR census section (gpu_census.cpp; the Altered sections time Elite's own draw and are excluded).
- The time between two consecutive stamps is one segment. It is attributed to the targets bound when it began, and carries the
  draws, dispatches and clears issued inside it. A segment with none of those carries its time into the next one.
- Draws are counted at the draw thunks (Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced, DrawAuto, and the indirect pair).
  Their shader pairs (bindingShaderHash VS and PS) are tallied per segment; the top three by draw count are written.
  Dispatches tally the compute shader's hash, looked up with lookupShaderHash at the dispatch (v2: bindingShaderHash(Cs) was
  never set, so the older CS column read 0). A zero hash means the shader registry never met that compute shader.
- At the next boundaries the scope's result is polled, then the stamps are read with DONOTFLUSH. Nothing blocks.

Reused: the shared DisjointClock and its frame/standalone scope rules (gpu_disjoint_clock.h, gpu_timing.cpp); the census's
section brackets (gpu_census.h, gpuCensusBegin/End); the binding shadow (bindingGet, bindingShaderHash, the per-slot
resolve of views); the draw thunks' owner-context checks; the config's log directory and Log::note. Added: scopeOpen() on
DisjointClock, the pass-scope functions in gpu_timing, the core header, and the module.

## Classification rules (gpu_pass_profile_core.h, classifyPass; first rule that holds wins)

Let E be the eye size (eyeW x eyeH, the same size vscreen publishes), and C be the first colour target.
1. Inside an EDVR census section: edvr.
2. Dispatches and no draws: compute.
3. No draws: clear if it cleared, else other.
4. Depth only (no colour), and the depth target is not E: shadow (v13.7: square or not, large or small; the 2048x1024 D16 pair and the
   256x256 D16 map were other before). With no E known, only a square map of width 512 or more, or an array of slices, is shadow.
5. Depth only and the depth target is E: scene_gbuffer (a depth prepass at the eye size).
5a. (v13.7) A colour target, at most four draws, and the most-drawn VS is a known HUD, holo or UI family: hud_overlay. The families are
   the game's: EyeDrawSnapshot::kHud (eye_draw_snapshot.h), kHoloGuiFxOffVs (holo_material.h) and kHoloTargetSphere (holo_families.h),
   matched by vertex shader hash; a pixel shader is not part of the match. Without them the same pass is fullscreen_at_eye.
6. (v4) A 252x252 single-target pass, or a dispatch-only pass whose top compute shader is B8E727E9 or F1FB2EEF: exposure (checked after clear, before the depth rules). 6. (v3) probe_cube: C is square, 256 to 1024 on a side (or a cube array of 6+ slices), and not at the eye size.
6a. (v2) C at E and the most-drawn VS is a light volume (0357BBB2DEE43C1F, 8289669D93A18C1D, 24DE25E496342EB8, A1B7CFCD0BE7493E,
   the instanced volumes of the lighting pass, from reviews\sealed-remaining-costs-2026-10-13.md): lighting_volume. Tested before
   rule 7, which would otherwise name the two-target lighting pass scene_gbuffer.
7. Two or more colour targets, C at E: scene_gbuffer.
8. One colour target at E: fullscreen_at_eye with one or two draws, otherwise forward_at_eye.
9. C smaller than E in area, with draws: post_smaller.
10. Otherwise: other.
Then, over the whole frame, any run of six or more consecutive single-colour, square, drawn-into passes of one size is
relabelled probe_cube (a cube map drawn face by face).

UI is NOT a class: no reliable target signature identifies it. Elite's UI draws that EDVR redirects are counted in the census
(AlteredUiLayer) and fall into the classes above here.

## Output

- edvr_logs\gpu_pass_profile_<YYYYMMDD-HHMMSS>.csv, one row per segment per frame. Columns: frame (0 to COUNT-1), pass (index within the
  frame), class, edvr (1 or 0), ms, draws, dispatches, clears, tris (estimate: count/3 x instances for triangle draws; 0 for the
  indirect and auto draws), colours, rt (WxH of C, or -), rt_fmt (DXGI format), slices, cube, dsv (WxH or -), dsv_fmt,
  top_vs_ps_draws (vs:ps:draws, up to three, hex), top_cs_dispatches (v13.7: the compute shaders bound at the pass's dispatches, most
  dispatches first, as hash(NAME):count in hex; NAME only for the two the repo names, APPLY_BIND_POSE_TRANSFORMS_CS and
  CLEAR_TRANSFORM_DATA_CS from skin_ledger.h; "-" when none).
- (v13.7) The class lines and the top groups print the median of one pass's ms beside the mean, so a single spike is visible as a spike.
- "gpu pass profile:" lines: "means over N frames: frame X ms = elite Y ms + edvr Z ms (unattributed U ms)"; one line per
  class with its ms and passes per frame and the class's median pass; "top K by median: M ms median  E ms mean  class=...
  draws=  passes=  rt ... colours ... slices ... depth ... shader ID", K = 1 to 20, where ID is vs:ps or cs:hash(NAME); one
  "top_cs_dispatches: hash(NAME)  N dispatches a frame" line per compute shader (top 5, all dispatches); and an overflow line
  when a frame hit its stamp cap.
- Medians are over the captured frames: one pass's ms per frame, pooled across frames within a group. Frames not read are
  left out of every number, and the frame line says why.
- The pool line (v2): "created the timestamp query pool for N frames (Q queries) in T ms". It appears once per capture size.
- Reading: elite plus edvr equals the frame by construction, so unattributed should be near zero. A nonzero unattributed value
  is time in a boundary-less stretch (CopyResource and the like are not boundaries; their time is in the segment they fall in).
- A CSV summariser is not written; the CSV is plain text and the numbers above are enough for the first read.

What the log shows when the capture does not run, so that "nothing" is not mistaken for "measured zero":
- No line after the press: the request was never taken at a boundary (check the hotkey line at start-up: "hotkey: GPU pass
  profile key bound"). A press during a capture says "refused".
- "this context does not own the GPU timing domain": the timing domain is not bound to this context.
- "frame N not captured: the shared GPU timing scope was already open": a census timer or the application-frame timing held
  the one scope. The batch stops there and the frames read so far are written.
- "frame N not captured: ..." with the disjoint reason (disjoint clock change, stale, driver failure): said with the scope's
  own reason. Disjoint frames are not zero; they are not read.
- "not ready after 240 boundaries": the GPU never reported the scope (about 2.7 s at 90 Hz).

Overhead (estimate, not measured): unarmed, one bool read per hook. Armed, each boundary costs one TIMESTAMP query insertion
on the owner context plus a bookkeeping step; a frame with N boundaries costs N insertions. Each census section and each
Draw adds its own bookkeeping, and the shape cache calls GetDesc and GetResource once per distinct view per capture. The
timestamp pool is 4 x 4096 queries, created on the first press (one hitch). Readback is one GetData per stamp, on a later
boundary. A measured figure needs the flight: compare the frame time in the batch with and without the press.

Risks:
- The app-frame GPU timing (gpu_frame_timing) yields its scope while a capture runs; its samples for those frames are dropped.
- Census timers borrow the profiler's scope while it is open, so their figures are not changed by it, but census lease budget
  (128 per record) is shared.
- A view address recycled within a capture would read a stale shape. Captures are at most 120 frames (30 by default), so the risk grows with the count; it is
  not checked.
- Boundaries merge the census's EDVR sections, so a Frame section that runs per draw adds a boundary per draw. The stamp cap
  (4096 per frame) is counted and said, not hidden.

## Interaction (v13.5, merge with the sealed-hangar trial)

The instruments share one DisjointClock (gpu_timing.cpp). A clock admits one open scope or frame lease at a time: a standalone scope (the profiler's per-frame scope, gpuTimingPassScopeOpen) is refused while any scope or frame lease is open (DisjointClock::scopeOpen), and a frame lease (GpuTimingFrameDriver::begin, the application-frame timing and the census spans) is refused while a standalone scope is open (startFrame).

- The sealed-hangar trial and the drawlist probe take no GPU timing. They count and read game memory with QPC and counters only, so they cannot contend with the profiler.
- The census (gpu_census.cpp, GpuIntervals over GpuTimer) and the application-frame timing (advanced.app_gpu_timing, default on in vscreen.cpp) do take the clock. A profiler frame that opens while either holds it fails with "the shared GPU timing scope was already open", and the frame is logged as failed. The profiler never corrupts or stalls them: its failure is local to its own frame.
- Conversely, a census or application-frame lease that opens during a profiled frame is refused for that interval.
- The v13.5 expectation (from the code): the profiler's boundary runs in the frame tick (device_hook.cpp, the vScreen rest), while the application-frame segment closes in hookedPresent after the real Present (gpuFramePresent), so a segment is open at the boundary and the profiled frame failed with that reason. v13.6 fixes this; see "The yield (v13.6)" below.

## The yield (v13.6)

While a capture is armed or running (from the press until the last profiled frame's scope is closed), the application-frame timing and the timers yield the shared clock:

- The press sets the yield flag (gpuTimingSetYield, gpu_timing.cpp). The flag is read by every new lease the frame driver and the timers would take: GpuTimingFrameDriver::begin, GpuTimer::beginImpl (standalone and borrowed). A refusal is counted and returns the same not-taken result a busy clock gives. The profiler's own scopes are not gated.
- gpuFrameCommand (gpu_frame_timing.cpp) returns before it opens an application frame while the flag is set. The frame is counted once (by its sequence). The rest of that frame's commands find no open frame, so its SubmitBegin records NoOpenFrame, as a frame that never began would.
- At the profiler's boundary, while the capture is armed or running, gpuFrameYieldBegin ends any application frame open across the boundary: an invalidate, which closes the open span through the ring (closeApplication, driver end, DisjointClock::finishFrame, which clears the open scope). Then the profiler's gpuTimingPassScopeOpen finds the clock free (scopeOpen() false) and opens its frame.
- The yield ends at the batch's end: the boundary after the last frame's close clears the flag (endYield) and logs once: "app gpu timing: paused for the GPU pass profile, N frames; census timer leases refused: M."
- Shutdown clears the flag.

Why this holds: an application frame opened before the press is closed at the first boundary by gpuFrameYieldBegin; every later frame is refused at its open, so no lease of theirs can be open when a profiled frame opens. The census's timers (GpuIntervals over GpuTimer) are refused the same way: a timer lease is only ever open inside a census section, and no boundary runs inside a section, so the census does not hold the clock across a boundary. If a census lease were open at a boundary, the profiled frame would fail with the existing reason and the flight would show it.

Verification: the clock behaviour is in tools\gpu_disjoint_clock_test (192 checks, including the sequence: frame open, scopeOpen holds and a second frame is refused; finishFrame releases; a standalone scope then opens, closes and releases). The capture's state is in tools\gpu_pass_profile_test (54 checks, including the yield's life across the press, the batch and the last close). The D3D path (the real query pool, the span ring and the hooked Present) is not modelled by any rig; its reasoning is above, and the flight is the check that a profiled frame opens with app_gpu_timing on (the "frame ... not captured" line should be absent, and the "app gpu timing: paused" line should count the frames).
