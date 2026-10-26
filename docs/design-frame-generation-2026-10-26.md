# Frame generation below headset rate (an Elite-specific OFXR-Bridge) — design, 2026-10-26

## Status

- **State (2026-10-26): DESIGN ONLY.** Nothing is built, keyed, installed or
  flown. Written from OFXR-Bridge at `d7fcca72` (v0.3.1) and EDVR at
  `6b8ef374`. No issue is filed yet.
- **Recommendation:** take OFXR-Bridge's frame loop, not its synthesizer. Elite
  renders at half the headset rate and EDVR's runtime presents every display
  period. The frame in between is **extrapolated** from Elite's newest frame.
  It uses the depth and motion vectors EDVR already makes for DLSS, the game
  camera, and the head pose predicted for that slot. v1 has no optical flow, no
  interpolation and no added latency. The frame is made on the game device
  right after the temporal pass, and the XR owner thread presents it one period
  later (## Recommendation, ## Design).
- **Why not a straight port:** OFXR-Bridge has been GPL-3.0 since v0.3.0 and
  EDVR is MIT, so no code is copied. Its synthesizer is colour-only optical
  flow, "depth-unaware" for translation, and cannot tell the cockpit from the
  world. It interpolates, which holds every real frame back at least one
  display period (## Why not a straight port).
- **When it runs (`auto`):** only while a measured gate holds. Elite is below
  headset rate, its frame fits in two periods with room for the synthesis, a
  shadow score says the prediction is clean, and there is no cut. v1 admits
  surface contexts only: on foot, the SRV, or a ship drawing planet surface.
  Space is admitted on Phase 1 evidence (## The gate).
- **Space:** use geometry, not flow. Stars and sky turn with rotation only. The
  cockpit moves with the head only. The world moves with the head and the
  extrapolated ship, movers with EDVR's engine-record vectors, and the UI on
  its own path. No two frames are blended, so nothing doubles. A Fill mode
  extrapolates only the slots Elite misses (## Space without artifacts).
- **Hypotheses (none tested):**
  - H1: surface frame drops fit half rate with room. Elite's work is under
    0.85 of two periods, less the synthesis.
  - H2: on surfaces, the extrapolation residual is near the DLSS
    frame-to-frame floor, except at disocclusions.
  - H3: in space, the far and cockpit classes predict to within that floor.
  - H4: interpolation (OFXR's model, with EDVR's motion) beats extrapolation
    by enough to pay for its latency. Expected false.
  - H5: synthesis on the game device costs at most 1.5 ms a pair at the
    flown sizes.
  - H6: a head pose predicted about two periods ahead leaves no visible
    error after the compositor's own rotation correction.
  - H7: the runtime stays at full rate while EDVR submits every period.
    VDXR's sticky half rate is the risk.
- **Phase 0 (no EDVR code):** fly stock OFXR-Bridge (colour-only route) over
  EDVR at a bright settlement and in space, with each runtime's own smoothing
  off and then on. This is the visual reference to beat.
- **Next flight: Phase 1, measure only.** It flies the shadow scorer (H2-H4 by
  pixel class), the surface classifier, the frame GPU span while armed (H1) and
  the pose prediction error (H6). Nothing shown in the headset changes.
- **Temporary key (proposed, not built):** `fix.frame_generation_shadow`
  (off | on), Phase 1 only. The user key `fix.frame_generation` (off | auto)
  comes with Phase 2. Its `on` value (forced, ignoring the gate's performance
  terms) is temporary, for A/B flights.
- **Declined in design (re-open only on new evidence):** ## Declined.
- **Open:** ## Open questions (canopy depth, early swapchain acquire, vendor
  half-rate stickiness, refresh-rate switching as an alternative).

## The ask

> I want you to build a design doc that would port a bespoke and Elite
> optimized version of OFXR-Bridge. The goal here would be to have it only run
> in situations where it performs well, bright scenery where Elite's frame rate
> drops below the target for the headset. In space this rarely happens. [...]
> If you have ideas around how to make it perform well in space without
> artifacts, that would be welcome as well.

(Sean, 2026-10-26.)

## OFXR-Bridge, as read

What was read: the source at
[`d7fcca72`](https://github.com/tig3rmast3r/OFXR-Bridge/tree/d7fcca72d569ec364281ab63ae29378bbfaf1251)
(v0.3.1, 2026-10-26) and at tag `0.2.1`
([`dad56aca`](https://github.com/tig3rmast3r/OFXR-Bridge/tree/dad56acafc6e1dde219940427738b926cf2ea555)),
the optical-flow-only core. Nothing was built or run. Paths below are in that
repository at `d7fcca72` unless they say 0.2.1.

| | |
|---|---|
| What it is | An OpenXR API layer that "inserts generated frames between rendered frames" (`README.md:3`). It is registered as an implicit layer under `SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit` (`include/xrfg/implicit_layer.hpp:40-41`), so it loads into every OpenXR app unless excluded. |
| Frame loop | The app's `xrWaitFrame` returns a doubled (or tripled) period (`src/layer/openxr_layer.cpp:2712-2718`), and its `xrBeginFrame` is answered locally. A presenter thread makes the real wait, begin and end once per display period and alternates the synthetic frame S with the real frame B (0.2.1 `src/layer/openxr_layer.cpp:2073-2090`, `:3035-3330`). |
| Synthesis | It interpolates between the previous real frame A and B, and shows S before B (`include/xrfg/three_slot_buffer.hpp:21-33`). The synthesizer takes an extrapolation fraction (`include/xrfg/d3d12_frame_synthesizer.hpp:183-188`), but the shipped scheduler interpolates, and an upstream extrapolation route was not imported (`docs/V445_UPSTREAM_INTEGRATION.md:36`). |
| Motion | By default, colour-only optical flow: NVIDIA's optical-flow hardware, or FidelityFX Optical Flow, on luma at 50% scale. A pack shader first removes the image motion from the OpenXR rotation and FOV change, so the flow sees only the residual (`src/d3d12/frame_synthesis.hlsl:482-543`). A midpoint solve then picks flow or camera-only motion per pixel (`:877-1050`). "Translation remains depth-unaware" (`d3d12_frame_synthesizer.hpp:203-205`). Optional routes: game vectors taken by hooking NGX DLSS Evaluate, a hybrid of the two, FSR 3.1 FG, and DLSS-G. |
| Graphics | Native D3D12. A D3D11 app is given a D3D12 session on the layer's own device and renders into shared textures (`openxr_layer.cpp:5705-5760`). |
| Head pose | By default S carries B's pose, which leaves black strips on the trailing edge during head turns (`openxr_layer.cpp:11138-11152`). An interpolated pose is opt-in. The newest head pose never reaches the synthesis. |
| Latency | B is held behind S. "Prefer FPS over latency", on by default, holds frames one more refresh: "about 11 ms at 90 Hz" (`docs/releases/0.2.4.md`). |
| When it runs | Always, once armed. It fails open when a submission has no projection layer, quarantines for 1 s after a layout change, and has a hotkey pause. By its own guidance it helps a game that "can't hold full rate", and the game needs headroom above half (`docs/TROUBLESHOOTING.md:66-72`). Vendor smoothing "fights OFXR" (`:69`). |
| HUD | On the default route, a HUD baked into the projection layer is flowed like the scene. |
| Known artifacts | "moving objects, disocclusions and head rotation" (0.2.1 `README.md:36`). FidelityFX "will produce artifacts during headset rotation in dark areas, this is known and cannot be avoided" (0.2.1 `README.md:79`). |
| GPU | The NVIDIA routes need Turing or newer with optical-flow hardware (not TU117, not Pascal). FidelityFX serves other GPUs (`README.md:32-34`). No millisecond cost is published. |
| Licence | GPL-3.0 since v0.3.0, commit `3085994` of 2026-10-25 (`README.md:279`). Tags up to 0.2.1 are LGPL-3.0-or-later. |
| Size | About 85k lines of C++20 and HLSL outside vendored code; `openxr_layer.cpp` alone is 17,225 lines. 148 commits since 2026-09-05; pre-releases only. |

Two of its lessons carry over regardless of the motion source:

- **A fail-open path must not keep handing a doubled period.** With no
  projection layer (a menu, a load) the app was paced once per presenter frame
  but told a doubled period, and the predicted time drifted a second per
  second. One session reached 637 s of lead and never generated again
  (`openxr_layer.cpp:6427-6437`).
- **Vendor smoothing must be off while the bridge runs**: SteamVR Motion
  Smoothing, Virtual Desktop SSW, Pimax Smart Smoothing
  (`docs/TROUBLESHOOTING.md:69`).

## What EDVR has, and what it lacks

- **The frame loop is EDVR's.** Elite's `WaitGetPoses` and `Submit`
  (`src/openxr/openvr_compositor.cpp`) reach `NativeRuntimeHost`. An owner
  thread (`owner_service.h`) makes every OpenXR call through `FrameBoundary`
  and `SessionState`. `FramePacer` (`frame_pacer.h`) can own `xrWaitFrame` on
  its own thread and hand Elite a synthesized predicted time
  (`frame_boundary.h`, `synthesizedTime`). This is the deferred ("turbo")
  pacing that `fix.weapon_stability` uses on foot
  (`src/d3d11/native_frame.cpp:469`).
- **A previous pair can already be presented again.** For transition-flash
  suppression the d3d11 half can withhold a frame, and the runtime then submits
  the kept pair with its old views and head pose (`native_runtime_host.h`,
  `sceneFinished`/`sceneLayerAvailable`, counters `replayedPairs`,
  `withheldPairs`). This is the plumbing a second, made pair would ride on.
- **One projection layer, colour only, on EDVR's own XR device.** Only the
  D3D11, time-conversion, refresh-rate, visibility-mask and performance
  extensions are enabled (`native_runtime_host.h:4054-4059`). No depth layer is
  submitted: it was set aside on 2026-09-24
  (`docs/openxr-performance-review-2026-09-14.md:33`). Each eye crosses from
  the game device by `SharedTextureTransfer`, and the two-device split is
  deliberate (the same review, "Keep the two-device architecture").
- **Depth and motion exist only on the game device, and only while
  `fix.temporal_aa` is not `off`.** The temporal pass holds, per eye, the
  colour, reversed-Z depth (0.025 to 50000 m), last frame's depth, the reactive
  mask, and R16G16 motion in render pixels from current to previous
  (`src/d3d11/temporal_pass.cpp:184-215`). EDVR makes that motion itself. The
  head delta covers pixels nearer than the 10 m ship split, the camera's rows
  cover pixels beyond it (`src/common/temporal_mode.h:41-58`), and terms are
  added for engine-record rigid movers, terrain, smoke, UI and supercruise
  planet patches. It is validated:
  `docs/kinematic-motion-injection-2026-09-19.md` (Status: certified
  engine-record deltas; raw-image warps agree with the engine motion to a quiet
  median of 0.05 input px). The open gaps it lists (walkers, some ships, the
  Coriolis brace's coverage) are the movers class below.
- **Context:** `SeatContext` (`where=aboard|on_foot|unknown`) and the
  Status.json flags in `src/d3d11/journal_watch.h` (supercruise, docked, on
  foot, SRV). Loading has `loading_state.h`. A planet patch's colour pass is
  recognised by its vertex shader (`src/d3d11/celestial_motion.h:29`). There is
  **no planetary-surface classifier**.
- **Timing:** late periods (`FrameBoundary::lateFrames`), the frame-cycle
  windows (`frame_cycle_stats.h`), the slow-regime observer (`slow_regime.h`,
  which names who held the time), and the display rate from
  `XR_FB_display_refresh_rate`. The app's GPU span exists but sits behind
  `advanced.app_gpu_timing`, which is off by default (`edvr.ini:1610`).
- **The deficit is where Sean says it is.** Stock fpsVR at one settlement:
  about 2 ms CPU in space, 9.5 ms in the settlement cockpit
  (`docs/engine-render-performance-2026-09-19.md`, 2026-09-21). A Pimax 8KX at
  4100x3212 per eye: producer 11.9-13.4 ms at the settlement against 0.5-3.3 ms
  away (same doc, 2026-09-22 addendum 2).
- **Elite already runs at half rate there; the runtime fills the gap.** At that
  settlement SteamVR's predicted period read 22.222 ms (45 Hz). Over VDXR at 72
  Hz, a GPU-bound ship at about 41 fps put the period at 27.78 ms
  (`docs/headset-lock-vdxr-2026-10-02.md`). The question is therefore not
  whether to halve, but **what fills the other slot**.
- **Earlier decisions:** a frame-rate lock was declined because "SteamVR's
  motion smoothing already halves adaptively" (`docs/performance.md:148`).
  Turbo pacing is "not a performance lever" (`:147`). Optical flow as a motion
  source was declined in favour of instrument use
  (`docs/per-object-motion.md:141`). This design does not reverse the first
  decision. It replaces what fills the halved slot.

## What fills the missing frame today

None of these was flown for this doc. URLs marked (search) were found by search
and not opened, so re-check them before quoting.

- **Meta PC ASW** takes motion from historic frames, not from app vectors
  ([Meta,
  search](https://developers.meta.com/vr/documentation/native/pc/asynchronous-spacewarp/)).
  Depth enabled ASW 2.0 under LibOVR
  ([search](https://developers.meta.com/horizon/blog/developer-guide-to-asw-20/)),
  and EDVR submits none.
- **SteamVR Motion Smoothing** extrapolates from the last two frames, and Valve
  contrasts it with TV interpolation, which "adds latency". It is for headsets
  on SteamVR's own compositor ([Valve,
  search](https://steamcommunity.com/games/250820/announcements/detail/1705071932992003492)).
- **Virtual Desktop SSW** runs on the headset
  ([search](https://uploadvr.com/virtual-desktop-synchronous-spacewarp/)).
  VDXR's open source shows no app-visible control for it
  ([mbucchia/VirtualDesktop-OpenXR](https://github.com/mbucchia/VirtualDesktop-OpenXR)).
- **Pimax Smart Smoothing** generates intermediate frames when the PC cannot
  hold the rate ([Pimax,
  search](https://pimax.com/blogs/crystal-light-tutorials/pimax-play-tips-and-features-overview)).
  How it triggers and whether it uses depth was not found.
- **`XR_EXT_frame_synthesis`** is the cross-vendor version of what this design
  builds. The app hands the runtime motion vectors and depth for each view, and
  the runtime "may use this information to extrapolate the rendered frame into
  a future frame"
  ([spec](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/extensions/ext/ext_frame_synthesis.adoc)).
  In the Khronos runtime inventory at
  [`fa71884c`](https://github.com/KhronosGroup/OpenXR-Inventory/tree/fa71884c0e74e6a447760eb1524a078d6c31960f/runtimes),
  it is advertised by Quest standalone, Android XR, Pico and SteamVR on Steam
  Frame (Linux). It is not advertised by `meta_pc`, `meta_pc_dev_mode` or
  `valve_steamvr_windows`. **No Windows PC runtime EDVR flies offers it
  today.**

Every one of these sees the cockpit as still and the universe as moving. Each
must estimate the universe's motion from pixels, and stars on black give a
motion estimator almost nothing to track. EDVR knows that motion exactly, from
the game's camera rows and the head pose it gave the game. That knowledge is
the case for building this at all.

## Why not a straight port

1. **Licence.** OFXR-Bridge is GPL-3.0 (LGPL-3.0-or-later up to 0.2.1), and
   EDVR is MIT. Copying either would relicense EDVR, so this is a
   reimplementation of ideas, with no code or shaders copied.
2. **Architecture.** EDVR is already the OpenXR application and owns the frame
   loop. A layer between EDVR and the runtime would take over EDVR's pacing
   (deferred pacing, withhold and replay, the slow-regime observer). It would
   see only the final colour, and it would move EDVR's XR device onto a D3D12
   session.
3. **Motion source.** Colour-only flow is depth-unaware for translation, which
   matters for low flight, the SRV and on foot. It is weakest on dark, sparse
   content, which is space. It is NVIDIA hardware or D3D12 only: FidelityFX has
   a DX12 backend and no DX11 path ([FidelityFX SDK
   v2.3.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK),
   `Kits/FidelityFX/backend/`). EDVR's motion is D3D11-native, runs on any GPU,
   and is already validated against the engine.
4. **Latency.** Interpolation cannot show S until B exists, so B waits at least
   one period, plus OFXR's default hold of one more.
5. **Always on** against a game whose deficit lives in a few contexts.

**Kept from OFXR-Bridge:**
- the split between the app at half rate and a presenter at display rate;
- the virtual-period ceiling;
- failing open with no projection layer;
- the quarantine after a layout change;
- removing known camera motion first and solving only the rest;
- choosing per pixel between camera motion and other motion;
- the instruction to turn vendor smoothing off.

## Recommendation

Build an EDVR-owned frame synthesizer that **extrapolates** the in-between
frame from Elite's newest frame, using:
- EDVR's depth and motion vectors,
- the ship's motion taken from the game camera, and
- the head pose predicted for the synthetic frame's own display time.

Make the frame on the game device, inside the temporal pass, right after the
real frame is published. That is where the data already is, and it queues ahead
of Elite's next frame, so no GPU priority is needed. The XR owner thread
presents it in the next display period, using the pacing EDVR already has for
deferred waits and replayed pairs.

Run it only behind a measured gate, and prove it first in a measure-only phase
that scores the predictor against real frames. That phase costs one flight and
puts nothing new in front of Sean's eyes. Leave optical flow and interpolation
out of v1: re-open either only if Phase 1 shows its classes dominate the
residual.

## Design

### Motion model

Each pixel of Elite's frame B, per eye, gets a class. The class decides how the
pixel moves to the synthetic frame S, which is shown one period later at time
T_S.

| Class | Pixels | Moves with | Expected exactness |
|---|---|---|---|
| far | the far plane, plus what the motion shader already treats as far or celestial: sky, stars, distant bodies | head and ship **rotation** only | exact |
| cockpit | aboard, nearer than `kTemporalShipMetres` (10 m) | the head only (B's eye to S's eye) | exact, up to disocclusion |
| world | farther than the split, static | the head and the ship rig, extrapolated | as good as the rig's extrapolation |
| mover | engine-record motion differs from camera-only motion | world, plus the object's own residual motion carried forward | linear over one period |
| reactive | the reactive mask: smoke, dust, lasers, explosions, shields | treated as world static, and scored on its own | not modelled |
| UI | the `ui_quality` layer, or UI depth in the picture (holo panels, flight HUD, target sprite) | the head only | exact for ship-fixed UI. World-anchored brackets lag one period |

**The ship rig** is the game camera with the eye removed: the view rows for B
times the inverse of the eye EDVR gave the game for B. The rig for S is B's rig
carried forward by the rig's linear and angular velocity over the display times
of A and B. Elite's flight model is inertial, so a constant-velocity step over
one period should be close. The error shows under boost, collisions and
touchdown, which is failure class 7 below. On foot there is no rig: the split
is 1 mm (`kTemporalOnFootSplitMetres`), and the world takes the camera's
motion.

This is the algebra the motion shader already runs for DLSS (the head delta
inside the split, the camera's rows outside it), run forward in time instead of
backward. **One owner:** the class decision and the transforms come from the
motion shader's code, not a second copy of it.

### The synthesis pass

1. **Splat (render size).** Forward-splat B's depth into S's grid using each
   pixel's class transform. The nearest pixel wins: a UAV atomic max on
   reversed-Z depth, then a pass that records each winner's source. The result
   names, for every pixel of S, the pixel of B it came from.
2. **Holes (render size).** A disocclusion is filled from its farther neighbour
   (background pull), never from the nearer one.
3. **Resample (output size).** Read B's upscaled colour (the DLSS, FSR, XeSS or
   TAA output, before the UI layer) at the mapped positions. S's pixels pass
   through one more resample than B's, so the filter must be sharp enough that
   a star keeps its energy, or stars soften every other frame.
4. **UI (output size).** Compose the UI layer with the head-only transform.
5. **Publish.** Copy the pair to a second `SharedTextureTransfer` set with S's
   views.

There is one source per pixel. Two frames are never blended, so stars, lasers
and HUD lines cannot double. Extrapolation's worst case is a held or stretched
pixel at an edge. Interpolation's worst case is a ghost.

### Where it runs

On the game device, in the temporal pass, right after B's output is published:
- **The data is already there.** Depth, motion, the reactive mask, the camera
  rows and the UI layer all live on the game device. None of them crosses to
  the XR device.
- **Ordering does the scheduling.** S's work queues on Elite's immediate
  context ahead of Elite's next frame, so it needs no second-device GPU
  priority. That priority is murky on D3D11. `SetGPUThreadPriority` has a
  documented absolute mode, but its effect under hardware GPU scheduling was
  not found
  ([docs](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/dxgi/nf-dxgi-idxgidevice-setgputhreadpriority.md)).
  D3D12's `GLOBAL_REALTIME` queue needs a privilege
  ([docs](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/d3d12/ne-d3d12-d3d12_command_queue_priority.md)).
  The public NVAPI has no context priority
  ([NVIDIA/nvapi](https://github.com/NVIDIA/nvapi)).
- **S's pose** is the freshest prediction for T_S available at the temporal
  pass, from the path that already answers Elite's "now" head queries
  (`pose_cache.h`, `now_head_publication.h`). The runtime adds those views to
  the frame input. `EdvrNativeFrameInput` is versioned and only ever extended
  at the end (`src/common/native_frame.h`). The compositor's own correction
  takes the remaining rotation at display, and H6 measures what is left.
- **Cost:** S adds its GPU time to every real frame it is made for. In Lock
  that is against a two-period budget.

**Rejected for v1:** synthesizing on the XR device at the slot. It needs depth
and motion carried across devices, and a priority D3D11 does not cleanly offer.

### Pacing: Lock and Fill

**Lock (2:1, Phase 2).** FramePacer owns `xrWaitFrame` for every slot, and the
owner thread runs `xrBeginFrame` and `xrEndFrame` for every slot. Elite's
`WaitGetPoses` returns once per two periods, with the predicted time of the
slot its frame will be shown in.

| Slot | Shown | Made on the game device | Elite |
|---|---|---|---|
| T | B, rendered for T | S_B for T+P, after B's temporal pass | starts C, told T+2P |
| T+P | S_B | (nothing) | renders C |
| T+2P | C | S_C for T+3P | starts D, told T+4P |

- **B late:** the slot gets the existing replay path (the previous pair with
  its own views), counted as `late_real`.
- **S missing:** B is replayed with its own pose, and the compositor turns it.
  Counted as `late_s`.
- **Entering or leaving Lock** happens on a slot boundary. The predicted times
  stay monotonic through the floor `FrameBoundary` already applies.
- **Fail-open:** with no projection layer (menus, loads), Elite is paced and
  told one period, never a doubled one. This is OFXR's drift lesson.
- **Deferred pacing:** Lock subsumes it. Both own `xrWaitFrame`, so while Lock
  is engaged it is the pacing and `fix.weapon_stability`'s on-foot deferral
  stands aside.
- **Latency:** each real frame is shown at the time Elite was told. Against
  today's runtime-halved stretches nothing is added. Against full rate, the
  world updates at half rate, with predictions in between.

**Fill (Phase 4, an experiment).** No lock. The presenter shows each real frame
when it arrives. When a slot's deadline passes with no new real frame, it shows
the S made from the newest one. This is for space and other contexts that
rarely miss. S must be made for every real frame while armed, so Fill arms only
when the recent miss rate is above a small threshold.

### The gate

The gate is a pure function of measured inputs, re-evaluated once a second,
with a 5 s minimum dwell each way.

**Engage Lock when all of these have held for 2 s:**
- temporal AA is on and valid this frame (depth and motion exist);
- Elite's real rate is below 0.9 of the display rate, or the runtime has
  already halved its predicted period;
- Elite's frame fits: GPU span plus S's cost, and CPU work, are each under 0.85
  of two periods;
- the context is admitted (v1: surface);
- the shadow score for the last 2 s is under its thresholds;
- there has been no cut in the last 2 s: a load, a withheld frame, the jump
  tunnel, or a `GuiFocus` change to a map or menu.

**Disengage when any of these happens:**
- Elite's work is under 0.75 of one period for 3 s, so it could hold full rate;
- the shadow score is over its thresholds for 0.5 s;
- a cut;
- motion is invalid;
- Elite misses its two-period budget on more than 5% of frames over 2 s (3:1 is
  not offered);
- the runtime halves its period while Lock is engaged, so vendor smoothing is
  fighting it.

**Notes on the inputs:**
- **Surface (new):** on foot, or in the SRV, or aboard with a planet patch's
  colour pass drawn this frame and not in supercruise.
- **The thresholds** (0.9, 0.85, 0.75, the score limits) are placeholders that
  Phase 1 sets. They ship as constants, not keys.
- **Brightness is not an input.** Bright scenes suit OFXR because flow needs
  texture and contrast. EDVR's motion does not care about either, so the gate
  measures prediction quality directly. On planet surfaces the two agree, and
  in space they need not.

### Space without artifacts

Space is where flow is worst and geometry is best:
- **Far field.** Sky, stars and distant bodies move by rotation alone, which is
  known exactly. They need no motion estimate, so dark, featureless and
  point-like content costs nothing. OFXR's FidelityFX route documents exactly
  that failure: "artifacts during headset rotation in dark areas".
- **Cockpit.** Rigid in the ship, it moves only with the head. Its only error
  is disocclusion behind struts and frames under head translation, which is a
  few pixels at one period.
- **World.** The head plus the ship, with the ship extrapolated. Fast turning
  is rotation, and rotation is exact in the far field.
- **Movers** (ships, stations) by engine-record motion. Lasers, explosions and
  particles are reactive: they are held where they were, and never doubled.
- **Stars must keep their energy.** The synthetic frame's far-field resample
  must match the real frame's sharpness, or stars pulse at half the refresh
  rate. The scorer measures it as a far-class high-pass energy ratio, synthetic
  over real, which must be about 1.
- **The HUD rides the head path,** never the world's. World-anchored brackets
  lag one period at half rate, which is what they already do at 45 fps.
- **Cuts are never synthesized across:** hyperspace, the transition-flash
  withholds and supercruise exit. Extrapolation is single-source, so a cut can
  at worst show one stale frame, never a blend of two scenes.
- **Fill mode suits space.** Space rarely misses, so the right tool there is to
  extrapolate the odd missed slot instead of letting the runtime repeat a
  frame. It adds no lock and no latency, and does nothing when it is not
  needed.
- **One question to answer from an eye dump before Phase 3:** does the canopy
  glass write depth? If it does, stars seen through it sit nearer than 10 m and
  would ride the cockpit path, frozen to the ship in a turn. The DLSS motion
  would have the same problem, so the motion shader may already handle it.

### Planets and on foot

- Static terrain and buildings are exact, except at disocclusions. Low flight
  and the SRV have the most parallax. The background pull covers one period's
  edge. A second source (A, reprojected) is the upgrade if the disocclusion
  share dominates Phase 1's residual.
- Thruster dust and smoke are reactive (the motion shader's smoke term). NPCs
  are movers, and walkers are a known gap in the engine-record motion
  (`kinematic-motion-injection`, "Other open work").
- On foot, the first-person weapon and hands move with the camera. They are
  movers with certified motion, or the class's residual will show them.
- The on-foot composite route (`vr_world_route.h`) and native on-foot VR differ
  in what depth the eye image carries. Phase 1 scores both.

### Vendor smoothing, and the forward path

- While `auto` is set, each runtime's own smoothing should be off.
  `docs/troubleshooting.md` gets the per-runtime steps: SteamVR per-app Motion
  Smoothing, Virtual Desktop SSW, Pimax Smart Smoothing, and Meta ASW in the
  Oculus Debug Tool. While engaged, a halved predicted period is logged and
  disengages Lock.
- **The synthesizer's inputs are `XR_EXT_frame_synthesis`'s inputs.** Per view,
  the extension takes a motion image, a depth image with near/far and a
  reversed flag, and an app-space delta pose. Its motion runs from previous to
  current, in Vulkan-style NDC, 3D by default. EDVR's motion runs from current
  to previous, in render pixels, so the conversion is a sign and a scale. When
  a Windows runtime advertises the extension, EDVR can submit those images and
  let the runtime synthesize, keeping its own synthesizer as the fallback.
  EDVR's seated space is the cockpit, so the delta pose is identity, and the
  ship's motion is in the vectors.

## Failure classes and their signatures

These are enumerated before building, so that Phase 1's one flight can separate
them. "Residual" is the shadow scorer's per-class mean and p95 of post-tonemap
luma error, against the floor.

| # | Class | Where expected | Signature | Mitigation |
|---|---|---|---|---|
| 1 | Disocclusion | cockpit struts under head translation; building edges in low flight, the SRV, on foot | residual concentrated at depth discontinuities; disocclusion share | background pull; second source |
| 2 | Unmodelled movers | walkers, some ships, the Coriolis brace | mover/world residual where engine-record motion is absent | the kinematic arc's open items |
| 3 | Reactive content | lasers, explosions, dust, smoke, shields | residual on reactive-mask pixels | held; accept, or residual flow (Phase 5) |
| 4 | Transparency and UI | canopy glass, holo panels, target sprite, world-anchored brackets | UI-class residual; far colour behind near depth | head path; the canopy check |
| 5 | View-dependent shading | specular, sun glare and bloom, lens flares | broad low residual, correlated with the sun in view (`docs/sun-glare.md`) | accept |
| 6 | Resampling shimmer | stars, thin lines, text | far-class and edge high-pass energy ratio away from 1 | a sharper resample filter |
| 7 | Rig acceleration | boost, collisions, FA-off flips, touchdown, camera shake | far/world residual against the rig's angular acceleration | a higher-order step, or disengage on spikes |
| 8 | Timing | the GPU saturated, Elite over budget, vendor halving | `late_real`, `late_s`, halved-period counts | the gate |
| 9 | Stereo mismatch | any class | per-eye residual difference | the same transform for both eyes |
| 10 | Jitter | DLSS input jitter left in depth and motion | a static-scene residual above the floor | remove the Halton offset when mapping render to output pixels |

## Budget

- **GPU:** unknown, and that is H5. The target is at most 1.5 ms a pair at the
  flown sizes against a 22.2 ms half-rate frame at 90 Hz. The scorer works at
  quarter size, with a target of at most 0.3 ms. For scale only: NVIDIA's own
  FRUC is quoted at 4.41 ms per 1080p interpolated frame on a 4090 ([NVIDIA
  application note,
  search](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note/index.html)).
  A VR pair is several times that many pixels.
- **VRAM:** S is an output-size RGBA8 pair, held on the game device, in the
  shared transfer and on the XR side. At 2600x2514 per eye that is 52 MB a
  copy, about 160 MB for three. At 4068x4016 it is 131 MB a copy, about 390 MB
  for three. The render-size splat buffers add one R32 surface per eye.
- **Latency:** none added in Lock or Fill (## Pacing). Interpolation would add
  at least one period, 11.1 ms at 90 Hz.

## Config

- `fix.frame_generation` = `off` (default) | `auto`. The menu label is
  "Frame generation". The value names what the user gets. `on` is a temporary
  A/B value that forces Lock in admitted contexts, ignoring the performance
  terms, and goes when the arc closes.
- `fix.frame_generation_shadow` = `off` | `on`, hidden and temporary, for
  Phase 1 only.
- There is no space toggle. A context is admitted by evidence, and a fix that
  always helps gets no switch.

## Phases and decision gates

1. **Phase 0, references (no EDVR code, one session).**
   - Fly stock OFXR-Bridge 0.3.1 on its default optical-flow route over EDVR,
     with vendor smoothing off. Do not use its game-vector route: it hooks
     NGX DLSS Evaluate inside the game, where EDVR's d3d11.dll calls it.
   - Scenes: a bright settlement, supercruise near a star, a station
     interior.
   - EDVR's frame-cycle windows at 22.2 ms show the layer engaged under
     EDVR's session.
   - Then fly the same scenes without OFXR and with each runtime's own
     smoothing on.
   - **Gate:** none. It sets the bar the bespoke work must clear. If the
     layer does not load under EDVR, record why: it is a reference, not a
     dependency.
2. **Phase 1, measure only (`fix.frame_generation_shadow = on`).**
   - The scorer: every real frame, at quarter size and per class, it
     extrapolates A to B and interpolates A and C to B (scored one frame
     late), both with EDVR's motion.
   - The floor: measured with the head still while docked.
   - The surface classifier.
   - The frame GPU span while armed, without the per-draw queries.
   - The pose prediction error: S's views at synthesis time against the
     views located for T_S.
   - A `frame generation (shadow)` line once a second, with an
     `edvr_log.py --health` tag. A run with no such line reads NEVER
     APPLIED, which tells it apart from a clean score.
   - **Flight:** a settlement in a ship, low flight, the SRV, on foot in a
     settlement, supercruise near a planet, a station interior, and a
     combat zone if one is reachable.
   - **Gate:** H1-H4 and H6 decided per context, and the gate's thresholds
     set from the floor.
3. **Phase 2, Lock on surfaces (`fix.frame_generation = auto | on`).**
   - Build the presenter, the S publication, the ABI extension and the
     gate.
   - **Flight:** an A/B at one settlement, off (vendor smoothing at its
     default) against `auto`. Log slots synthesized, `late_real`, `late_s`,
     engage and disengage with reasons, and vendor halving.
   - **Gate to ship:** Sean prefers `auto`, `late_s` is under 1% of
     slots, there is no vendor halving while engaged, and Lock disengages
     within 1 s of a cut.
4. **Phase 3, space,** if Phase 1's space classes pass: the star energy check,
   and the UI path for every context.
5. **Phase 4, Fill mode,** as an experiment.
6. **Phase 5, only on evidence:**
   - residual optical flow after the full geometric pre-warp, if classes 3
     or 5 dominate on surfaces (the licence question below comes first);
   - or the `XR_EXT_frame_synthesis` hand-off, once a Windows runtime
     offers it.

## Tests required before a flight

- **A synthesis-math rig.** A CPU reference of each class transform against
  synthetic scenes with known head, rig and object motion, to under 0.1 px. It
  covers a reversed-Z far-plane case, the 1 mm on-foot split, and jitter
  removal.
- **The gate as a pure function,** table-driven: engage, each disengage reason,
  the dwell, a cut, and fail-open.
- **The presenter's slot schedule,** in the style of `native_frame_test`:
  steady state, late real, late S, a cut, engage and disengage. Predicted times
  stay monotonic, and a fail-open path never hands a doubled period.
- **ABI:** an old d3d11.dll against the new runtime, and the reverse.
- **The config contract** (`tools/check_config_contract.py`) for the new keys.

## Declined

These are declined in design, not by flight. Each one names what would re-open
it.

- **Copying OFXR-Bridge code or shaders:** the licence.
- **Optical-flow-first synthesis:** flow is weak on space content, and EDVR has
  better motion. Re-open if Phase 1 shows classes 3 and 5 dominate on surfaces.
- **Interpolation as the default:** the latency. Re-open if H4 holds by a clear
  margin.
- **3:1:** extrapolating two periods ahead from a game below a third of the
  rate is out of scope.
- **Synthesis on the XR device at the slot:** see ## Where it runs.
- **A brightness gate:** brightness is flow's proxy for prediction quality, and
  the gate measures prediction quality directly.
- **Depth submission to the vendor as the fix:** it was set aside on
  2026-09-24, and the vendor still would not know the ship.

## Open questions

1. Does the canopy write depth (## Space)? Answer from an eye dump.
2. Can S's swapchain images be acquired early, so the copy happens when S
   arrives rather than at its slot? This needs the OpenXR swapchain acquire
   rules checked.
3. Does each runtime return to full rate once EDVR submits every period? VDXR
   held exactly half rate for minutes at a time, and SSW engaging on the game's
   stalls and staying on is that doc's suspected cause
   (`docs/engine-render-performance-2026-09-19.md`, 2026-09-22 addendum).
4. Is a lower refresh rate (`XR_FB_display_refresh_rate`, already enabled) an
   artifact-free alternative for some headsets? The spec makes it only a
   request, and says lowering gives "higher latency and flickering"
   ([spec](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/extensions/fb/fb_display_refresh_rate.adoc)).
   Which rates each runtime enumerates and honours was not found.
5. Licence, if flow is ever added: NVIDIA's Optical Flow SDK licence is
   reported to forbid use that makes it subject to an open-source licence
   ([search](https://docs.nvidia.com/video-technologies/optical-flow-sdk/license/index.html)).
   The D3D12 interface headers OFXR vendors carry MIT-style terms
   (`licenses/NVIDIA-Optical-Flow-Headers.txt`). Resolve this before Phase 5.

## Environment

- **Applies to** EDVR's native OpenXR runtime on Windows, on whichever OpenXR
  runtime it selects: SteamVR, Meta PC (Link and Air Link), VDXR, or Pimax
  OpenXR. The game is D3D11 with temporal AA on (DLSS, FSR 3.1, XeSS-SR or
  TAA), because depth and motion exist only then. It works on any GPU vendor,
  since there is no flow.
- **Does not apply to** the flat profile (Proton, macOS) or Elite's own LibOVR
  and OpenVR routes, which EDVR's runtime bypasses.
- **Per-eye sizes on record:** Pimax Crystal Super flown between 2016x2017 and
  2604x2644; SteamVR at 4068x4016; Quest 3 from 1824x1968 base
  (`docs/openxr-resolution-per-headset-2026-09-14.md`) and 1536x1632 on foot
  (`docs/design-native-onfoot-vr-2026-10-13.md`, F95).
- **Refresh rates:** 72, 90 and 120 Hz runtimes have all been seen. At 72 Hz,
  Lock means 36 real fps, which is the same as today's halved stretches there.
- **No fixed-size tables** are planned beyond the one S pair.

## Journal

### 2026-10-26: design written

- **Read:**
  - OFXR-Bridge's source at `d7fcca72` and 0.2.1, not built;
  - EDVR's frame loop, temporal pass, context and timing code at
    `6b8ef374`;
  - the platform sources linked above. Vendor documentation hosts were
    unreachable from the session, so Meta, Valve, Pimax and NVIDIA
    behaviour is from search results, marked (search). The GitHub-hosted
    specs, the Khronos inventory, Microsoft's API docs, the FidelityFX SDK
    and NVAPI were read.
- **The finding that shaped the design:** Elite already runs at half rate where
  it struggles, because SteamVR and VDXR halve the period. So the feature is a
  better filler for the slot the runtime already leaves. EDVR is the only party
  that knows the ship's motion, the cockpit, the HUD and the depth, and an
  extrapolator built on those adds no latency.
- **Next:** Sean's call on Phase 0, and the Phase 1 build.
