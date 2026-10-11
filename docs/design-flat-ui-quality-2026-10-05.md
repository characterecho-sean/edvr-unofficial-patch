# Flat UI quality with TAA DLSS and FSR

## Status

- **State: SHIPPED (2026-10-09); B390 cockpit proof gap fixed locally (10-11).**
  Steam flat XeSS/UI 125: user reports HUD and menus stable. Correct 4b7bfec4
  proves both E894 variants and composites every steady frame. The October 11
  entry records physical input proof, the fix flight and temporary-probe cleanup.
  Flown OK on Epic flat. Branch
  `claude/flat-display-crash-ui-smearing-f1d96a`; the census was removed in the
  commit that follows the flown build.
  - SS 1.0 at 125%, HDR route: build `eced5813`, Sean: "Looks great!". In the
    three steady cockpit windows, frames = copies = door-armed = tone-admitted =
    composites = 8046; no back-offs, no declines.
  - SS 0.5 at 100%, copy route: flown OK on `75cd495c` (every window 2700 on
    every counter, Sean: "works great"). Copy route last flown on 75cd495c;
    eced5813's flat admission change not yet flown there.
  - Not flown: 100% at SS 1.0.
- **Local flight:** correct 4b7bfec4, 120427.log, SS 0.5 then about 0.65/UI 125.
  Cleanup keeps the flown guards; 88 CPU checks pass. The UI
  rig's 206 Wine failures exactly match the clean baseline. Full build stops
  at baseline run_jobs failures; native Windows validation remains.
- **The key:** `fix.ui_quality = off | 100 | 125`, default 100 in flat (the
  code's fallback and the flat ini template), the fourth flat F8 row. 100: the
  panels at display size and the cockpit HUD out of temporal history, whatever
  the render scale; 125: the same 1.25x larger; off: today's frame. The panels
  and the cockpit HUD layer need the flat anti-aliasing on.
- **What ships:** the panel factor `f = (R/D)/T` (ui_panel_scale.cpp, "ui
  quality: panels (flat)" lines); the flat UI layer (flat_ui_layer.{h,cpp}, a
  mono adapter over ui_layer.cpp) for the holo panels, flight HUD and target
  sprite, its "flat ui layer" 30 s lines and refusal counters. The holograms
  family is not taken (only one of its seven pairs has camera rows in flat;
  the 13:51 loading-screen ghost): it stays in the frame as before the port.
- **Removed:** the flat UI census (flat_ui_census.{h,cpp}), a temporary
  instrument; its target-class and family rules live on in
  flat_ui_layer_math.h.
- **Known leftovers:**
  - the holograms stay in the frame, through the anti-aliasing (six of seven pairs
    carry no camera rows flat reads: 9B34C331902DC1ED, DF3503CD07F9B10C,
    5453D19B6D362364, A2C2D5510BF1926D, 9611A454527F7FEB, B932058F26B76691);
  - flight HUD `other-shift`: rare, a camera shift of 2x the frame's y phase;
    left in the frame;
  - the flat resize-loop fix (771bb99a): exercised OK by windowed resizes on
    13:23 and 13:51 (`result=ok refs=0`).
- **Ruled-out pointer:** the 2026-10-09 entries below; VR's retired deferred
  UI replay ([crisp-ui-handoff.md](crisp-ui-handoff.md)).
- **0.19.0 review (10-09), finding 3:** the Supersampling setter's panel factor is
  held until the scene's size follows it, its epoch check and write one operation
  under the floats' lock (last entry; built, not flown).
- **System Map families (10-10, built, not flown):** `kMapCanvas` and `kMapSprite`
  (flat only; asked while GuiFocus 6, 7 or 8 is open, on the HDR target) go into the
  flat UI layer like the HUD families, so they are drawn at D x T after the upscale.
  The "flat ui layer map:" 30 s line counts them and names each refusal. Entry at the end.
- **Temporary config keys:** none.
- **Next locally:** no repeat flight needed for capture cleanup. The installed
  4b7bfec4 rendering path is verified; cleanup 02f50bd7 is pushed and compiled.
  Keep codex/xess-flat separate pending the user's Windows full build/testing.
- **Next on Epic:** fly SS 0.5 on the shipped build (the copy route under eced5813's
  admission change) and 100% at SS 1.0; then an Epic flight opening all three maps.

## Purpose

Bring VR's source-panel quality and separate UI composition to flat Elite:
retain fine text below display-resolution world rendering; avoid UI history
trails; retain extra spatial samples above 1x before conversion to display.

This targets the flat `d3d11.dll` profile and EDVR's TAA, DLSS/DLAA and FSR
backends. Qualification records display/render size, GPU, game build, backend
versions and graphics mods. Elite's spatial FSR path is outside this change.
The proposed mono adapter has no VR-runtime or HMD-sizing dependency.

## Current paths and reusable work

The existing code has several different boundaries. Treat them separately:

| Path | Current behaviour | Design consequence |
| --- | --- | --- |
| Flat UI after the game's output copy | Runs after EDVR resolve and RCAS; see `FlatRuntimeDrawScope` in [flat_runtime.cpp](../src/d3d11/flat_runtime.cpp) and [flat_sharpen.h](../src/d3d11/flat_sharpen.h). | This UI already avoids temporal colour filtering. First check its source-panel resolution and raster phase. |
| Flat late HDR overlay | [FlatOverlayLayer](../src/d3d11/flat_overlay_layer.h) retains a clean HDR image and coverage; [flat_mono_resolve.cpp](../src/d3d11/flat_mono_resolve.cpp) restores raw covered fragments at the render-sized HDR grid. | Useful isolation and fallback machinery, but it does not render UI at the proposed UI size. Do not extract the same family twice. |
| Flat HDR resolve route | Resolves before the game's bloom, depth of field and tone chain; `FlatRuntimeDrawScope` also supports the output-copy route. | A layer attached only to the output-copy resolve misses the HDR route. |
| VR UI layer | [ui_layer.cpp](../src/d3d11/ui_layer.cpp) handles post-tone and HDR HUD families at their draws. | Reuse verified family rules and math with mono sizing and scheduling; do not use retired deferred replay. |
| VR panel sizing | [ui_panel_scale.cpp](../src/d3d11/ui_panel_scale.cpp) changes the engine's panel-size divisions, so movie layout, viewport, colour and depth agree. | Reuse the checked patch mechanism only after proving the flat inputs and resize path. Enlarging a texture alone cannot supply sharper glyphs. |

VR handles draws as issued and reissues the game's tone draw at its boundary.
Flat excludes `fix.ui_quality` in
[runtime_profile.h](../src/common/runtime_profile.h); proposed flat behaviour
is automatic with supported temporal AA, independent of the VR percentage.

VR qualification and gaps are in
[ui-layer-2026-09-23.md](ui-layer-2026-09-23.md) and
[cockpit-hud-layer-design-2026-09-27.md](cockpit-hud-layer-design-2026-09-27.md);
they do not prove flat coverage.

## Resolution policy

Use four named extents, taken from the verified active frame and view:

- **R:** the game's actual scene render size after supersampling.
- **E:** the temporal backend's negotiated evaluation size.
- **D:** the active display/back-buffer output size, including windowed mode.
- **U:** the UI raster size chosen by this design.

For the currently supported uniform scaling relationship, choose `U = D` when
`R <= D`, and `U = R` when `R > D`, in both axes. Use the rounded resource
dimensions, not a second calculation from an INI supersampling value. Validate
aspect, viewport and output ownership before using the relationship; mixed axis
sizes, letterboxing or multiple views need an explicit view mapping.

[flatResolveRoute](../src/d3d11/flat_mono_resolve.h) currently evaluates TAA at
D even when R exceeds D. Supersampled DLSS/DLAA and FSR evaluate at R.
[flatDlssNegotiate](../src/d3d11/flat_dlss_negotiate.h) can reduce E below D
when the input is below a vendor limit. None of these choices lowers U.

At D=2560x1440: R=1280x720 gives U=2560x1440; R=3840x2160 gives U=3840x2160 and
one spatial downsample. DLSS negotiation below its input limit may make E<D,
but U remains D.

The existing E-to-D route presents the world. UI renders separately at U and
converts to D; TAA at E=D does not require enlarging the world to U. No UI
history, temporal reprojection or RCAS follows UI composition.

## Panel source quality

Scaleform uses `s = min(c / 1920, d / 1080)` for truncated movie stage,
viewport, colour and depth sizes; see
[ui-sizing-owner-2026-09-23.md](ui-sizing-owner-2026-09-23.md).

**Unproven flat hypothesis:** identify the owner and actual flat c/d inputs,
their relationship to U, and the panel recreation path. They may already track
D. Only then adapt the checked operand patch at all four divisions to produce
U-derived stage size while preserving aspect, FOV and layout.

Movie layout, glyph atlas, viewport, colour/depth resources and cursor hit
testing must agree. A divisor change cannot resize an existing movie; exercise
startup and live changes without menu reopen. Enlarging an existing bitmap or
low-resolution panel does not sharpen it.

The checked build-332841 patch uses HMD/frustum inputs in `gatherInputs`, so
flat needs its own proven input mapping. Unsupported builds retain original
sizing; log any panel or D3D11 cap instead of claiming U.

## Frame composition

The proposed flow separates world colour from supported UI colour:

```text
world at R -> existing AA and world post/tone route -> world at D
UI sources at their U-derived stage size -> current-frame UI raster at U
                                         -> UI tone / colour conversion
                                         -> spatial U-to-D conversion
world at D + UI at D -> ordered native overlays -> Present
```

This is semantic order; find insertion draws for both flat routes and any
native UI interleaving. Preserve each route's current world AA boundary: before
post/tone on HDR, after tone on the output-copy route.

1. Freeze R/E/D/U, resource generation, camera phase, scene/depth lineage and
   output identity. Prepare before taking draws; a prior plan is a prediction.
2. Classify by family, target, sampled-resource lineage, camera, blend and
   depth. Verify menu/panel, HUD/hologram and marker families in flat before
   assigning each to existing overlay, native post-copy, or new layer. Leave
   unknown draws intact.
3. Handle taken draws immediately at U, restoring state and keeping colour out
   of world AA/history. Preserve required depth, stencil, query and UAV effects
   or refuse extraction. Avoid frame-end draw replay.
4. Remove UI raster jitter. Remap viewport, scissor, coordinates and current
   scene depth consistently to U. Preserve 3D marker camera/occlusion and 2D
   screen anchors; a resolver rejection stamp cannot substitute for depth.
5. Keep HDR UI separate through temporal AA, bloom and depth of field, but
   preserve intended UI glow. Convert with the captured game tone draw and
   current exposure; use a separate current-frame path for glow/background
   effects. Do not tone-map LDR UI twice or turn world background into
   coverage.
6. Convert U to D once in the correct colour space. Downsample premultiplied
   colour and coverage together; preserve alpha, additive and multiply blend
   semantics. Frosted panels sample mapped current scene without making their
   world background opaque coverage.
7. Preserve game menu, cursor, F8 and mod order. Native post-copy UI can stay
   at D when U=D; when U>D, prove its source/raster route to U. Compose at each
   verified order boundary, so a later native menu can cover taken HUD. Trace
   EDHM/ReShade through output copy and Present, including upstream mod calls.

The mono adapter owns scheduling; share pure size, coordinate, coverage and
blend helpers. Reuse `FlatOverlayLayer` isolation where suitable. Each family
has one owner, and each taken contribution appears once. Weapon/world
foreground isolation remains separate.

## Failure and resource lifetime

Keep an original presentation path available until clean world plus UI
composition has completed. A backend refusal, missing depth/tone binding, query
conflict, resource failure or ambiguous order must restore the original frame
and bindings, with one reason in the summary. Do not leave a UI-less world
after suppressing draws, show both versions, or reuse yesterday's layer. An
unknown UI family may remain in the original scene, but report incomplete
coverage rather than declaring that all UI bypasses AA.

Bootstrap with observation until the panel and frame contracts are known.
Resize, supersampling/AA changes, loading/view transitions, swap-chain/device
replacement and output-format changes invalidate affected layers and plans.
Clear coverage each eligible frame and retire resources through the existing
bounded retirement rules. Do not allocate or synchronously read back per draw.
Cache identity includes U and the view/output generation, independently of E.
AA-off restores original panel sizing through recreation as needed and uses the
original flat path; reactivation requires fresh frame/panel contracts.

Measure bytes and CPU/GPU cost at U, including above 1x. Seed world background
only when needed. No eligible draw means no UI GPU pass.

## Evidence before changes

Hypotheses: affected UI enters temporal history, inherits scene jitter, or
comes from a reduced panel. A single combined capture must distinguish them;
smearing alone cannot. Extend capture/census tools with their self-tests.

Proposed evidence and discriminating signatures:

| Question | Required evidence | Interpretation |
| --- | --- | --- |
| Did the intended path run? | Per frame/summary: selected route, R/E/D/U, mode, generation, eligible/taken/completed draws, composition count and refusal reason; print zero counts too. | Taken=0 or completed=0 distinguishes missing execution from a clean image. |
| Does a family enter AA? | Ordered draw ID, shader family, target and SRV lineage at UI draw, AA input, tone and final copy; labelled original/clean/layer/final images. | UI present in the AA colour input confirms contamination; post-copy-only UI refutes it for that family. |
| Is a panel undersized? | Actual flat c/d and stage dimensions at init/recompute, resulting movie/viewport/colour/depth extent, sampled panel identity and glyph atlas updates. | Scale tracking R below D confirms reduced source quality; scale already tracking U refutes this cause. |
| Is UI jittered or misregistered? | Raw and applied UI camera phase, viewport/scissor mapping, depth sample coordinates; static text and moving occluder crops. | A stationary edge following the scene jitter indicates wrong raster mapping. |
| Is ordering or coverage wrong? | Draw order around HUD, frosted panel, menus/cursor and mod passes; colour/coverage/depth crops. | Doubled UI, opaque background, leaked hidden marker or menu under HUD identifies the failing boundary. |
| Is the path affordable and stable? | UI shading/tone/seed/composite timings, CPU scope, allocated/retired bytes and resize/refusal counts. | Repeated allocation, waits or work with no eligible UI blocks qualification. |

Verify the log before interpreting any counter, using the sanctioned tool:

```text
python tools\edvr_log.py --target epic --expect-build HEAD --version
python tools\edvr_log.py --target epic --expect-build HEAD
```

Record `ruled out: X, because Y` here after a flight refutes a cause. Record
coverage by family and route, rather than converting a partial success into a
blanket statement about the UI.

## Implementation and acceptance

After ownership is measured, implement the mono size/panel adapter, then
missing family routing and composition. Qualify both resolve routes. Keep
Status current. Add no production UI toggle or threshold.

Extend `ui_quality_test`, `ui_holo_pass_test`, `ui_layer_world_test` and the
flat temporal/mono GPU rigs as appropriate. Cover R below/equal/above D, TAA's
E=D with R>D, DLSS E<D, rounded 16:10/ultrawide sizes, invalid mapping,
supported/unsupported panel patch, live recreation and cursor alignment. GPU
fixtures must prove UI colour stays out of AA/history while world AA runs.
Check alpha/additive/multiply, downsampling, depth, tone/exposure, query/UAV
refusal, no-draw frames, rollback, resize and state restoration.

Run the absolute-path full `build.bat` and require its receipt. Install and
verify with `tools\install_edvr.py` before an exact-build flat run.

Use one qualification route through main menu/loading, cockpit/station
services, panels/HUD/holograms, maps and on-foot UI with a moving background
and an occluder. Cycle TAA, DLSS and FSR at 0.5x, 1x and a supported value
above 1x; include a DLSS under-limit case, window resize and AA-off/on. Record
any unavailable backend or matrix cell instead of treating it as passed. Check
supported graphics-mod combinations and a VR regression separately.

Acceptance requires requested U and source-panel scale; no temporal UI trails;
stable anchoring, depth, colour, coverage and order; each taken contribution
once at its proper boundary; and continuous world AA. AA-off returns original
panel geometry. Thin moving geometry can still exhibit spatial aliasing without
temporal accumulation: removing temporal smearing does not prove all shimmer is
eliminated. Preserve the extra samples above 1x and diagnose remaining aliasing
from the captured geometry and raster phase. Record measured cost and every
remaining family gap before describing the feature as complete.

## 2026-10-09: implementation (built, not flown)

Environment: flat profile (no VR runtime), any backend (TAA, DLSS/DLAA, FSR),
game build 332841 (the panel patch is build-keyed), Epic test bed at D
3840x2160. Nothing below is flown.

**Phase 1, the panels (`3923a773`).** `fix.ui_quality` passes the flat gate
and is the fourth flat F8 row (the settings warning gives up one spare line:
11, ten needed). In flat the panel formula's c is the record's size with
k = 1, i.e. R with Supersampling in it; the census measured it (an rtt-init
panel 1920x960 at R 3840x2160, 960x480 at R 1920x1080). So the same four DIVSS
operands take `f = (R / D) / T` on the axis the game divides (height at 16:9
and wider, width below), solved by `uiPanelSolve` with no Supersampling term:
same [1/4, 1] clamp and 14336 px budget. At D 3840x2160, R 1920x1080:
T 1.0 -> f 0.5 (operands 540/960, panels x2, widest 3840 px); T 1.25 -> f 0.4
(432/768, x2.5, widest 4800 px). R at or above D x T -> f 1 (the floor). Frames
whose R is not D's shape (the 512x512 preview frames) make no factor. The
factor needs the flat anti-aliasing on (with it off the scene would only
minify larger panels, unfiltered). The Supersampling setter thunk scales the
flat plan by the setter's move. Orbit-line widths stay VR-only (gated on the
VR profile): in flat the lines are scene geometry upscaled with the world.

**Phase 2, the layer.** `flat_ui_layer.{h,cpp}` is a mono adapter over the VR
layer's own take, tonemap re-issue, door and composite (ui_layer.cpp, eye 0);
`flat_ui_layer_math.h` holds its pure rules (pinned in ui_quality_test). The
VR layer is live in flat with the key on and the flat AA on. The adapter hands
in what native_temporal and vScreen give VR: the flat frame number, the
draw's jitter, R. The families: holo panels, flight HUD, target sprite,
holograms (not the scene lines). Jitter: the draw's own camera rows are
measured (flatCameraMeasureRowShift); the frame's phase is cancelled when the
rows carry it, nothing when they carry zero, and anything else is refused
(`other-shift`). The door arms frame N+1 only when frame N was resolved by
EDVR to a D-sized picture; the layer is then D x T (VR's rule: the door's size
times the target, 125 box-filtered down by the composite). The composite runs
at the game's output copy after the resolve and RCAS, over whatever that copy
reads, so a frame armed and then refused still shows its HUD (over the game's
own R picture). The HDR route needs nothing of its own: it resolves H before
the tonemap, the HUD is out of H either way, and the composite runs at the
same copy. Every refusal leaves the draw in H as stock, counted by reason;
resize releases every layer reference (none is the back buffer).

**Flights 11:08 and 11:32 (2026-10-09).** 11:08 (130f62b0): the layer never
went live; the flat gate refuses `advanced.temporal_aa_jitter_sign`, a refused
key reads "off", so the jitter switches read as set (fixed in 2ec58b96).
11:32 (2ec58b96): live, holograms taken at R = D on the HDR route, no tonemap
admitted, and the session-long stand-down after 30 frames. Ruled out: (c) the
tone pair -- VR's admission is structural plus the HDR source's identity, not
a pair list. Holds: (b), in a different form: on the HDR route the post chain
copies H with the game's plain copy (vs DEF19B035D5EDEDC, ps DED8796049C7BB4A,
the route's own trigger) and the tone reads that copy, so VR's identity match
against H never fires; (a) in part -- the flat path offered only 3-vertex,
1-instance draws to the structural rule. Fix (built, not flown): the flat
admission keys on flat's own known tone pairs (any vertex count) and accepts
H or a plain copy of H; a take is made only when the previous frame's tone was
seen reading the HUD's target or its copy (`tone-unproven` otherwise); one
missed tonemap backs the HUD path off for 30 s, re-armed with a line, instead
of a session stand-down; the first eight tone candidates are logged.

**Flight 12:04 (75cd495c).** SS 0.5, copy route: the whole chain, every
frame (2700/2700 door, tone, composite; Sean: "works great"). SS 1.0, HDR
route: the first HDR-route frame's re-issue was declined ("bindings at the
re-issue are not the admitted draw's", vs/ps 0): on that route the resolve
runs inside the tone draw's own scope (treatHdr) and the binding shadow no
longer names the draw's bindings afterwards, so the shadow-based drift check
failed and the path backed off for 30 s -- the HDR-route frames were never
served. Ruled out: per-frame route alternation at R = D -- the 12:31:36
window's hdr=1780 copy=886 straddles the SS switch at 12:31:16 (copy before,
treated HDR route on 435-450 of 450 frames per 5 s after). Fix (built, not
flown): in flat the drift check reads the context, the admission carries the
pair the scope read before the resolve, and back-offs escalate 2-4-8-16-30 s.
Left: six hologram pairs (9B34C331902DC1ED, DF3503CD07F9B10C, ...) have no
camera rows at b1 in flat's camera table and stay in H; a flight HUD draw
measured twice the frame's y phase (other-shift).

Not done: draws after the HUD inside H (VR's known inversion applies: an
untaken draw issued after a taken HUD draw is now under it). The after-UI take
is not run in flat. The VR lines the shared code prints still say "left eye".

## 2026-10-09: the flights, and what each ruled out

All on Epic flat, D 3840x2160, DLSS/DLAA, EDHM chained (3Dmigoto,
`d3d11_edhm.dll` via `real_dll`).

- 09:36, 771bb99a (census only): the cockpit HUD families are drawn into H
  before the resolve, jittered; rtt panels follow R. ruled out: EDHM as the
  cause of the smear, because the census matched stock family hashes and the
  layer gate was the blocker.
- 11:08, 130f62b0: the layer never went live. Cause: the flat gate refuses
  `advanced.temporal_aa_jitter_sign`, a refused key reads "off", so the jitter
  switches read as set (the dead gate). Fixed in 2ec58b96
  (uiLayerJitterAsShippedFor; the not-live reason logged verbatim).
- 11:32, 2ec58b96: live; holograms taken at R = D on the HDR route, no
  tonemap admitted, session stand-down after 30 frames. Cause: on that route
  the tone reads a plain copy of H, and the flat path offered only 3-vertex
  draws to the structural rule (the tone copy). ruled out: tone variant list
  (c), because admission is structural. Fixed in 75cd495c (admission by flat's
  tone pairs, H or its copy; takes only behind a proven tone; back-off instead
  of stand-down; tone candidates logged).
- 12:04, 75cd495c: SS 0.5 copy route OK on every frame (2700/2700). SS 1.0
  HDR route: the first re-issue declined "bindings ... not the admitted
  draw's" (vs/ps 0) and backed off 30 s. Cause: the resolve runs inside the
  tone draw's scope and the binding shadow no longer names the draw's
  bindings (the HDR-route binding shadow). Fixed in eced5813 (flat drift check
  reads the context; the admission carries the scope's pair; escalating
  back-off 2-30 s). ruled out: per-frame route alternation at R = D, because
  the mixed window straddled the SS switch.
- 12:46, eced5813: SS 1.0 at 125%, HDR route, OK (8046 = frames = copies =
  door-armed = tone-admitted = composites in the steady windows; the loading
  window's 235 composites match its 235 tone-proven frames, nothing lost).
  Sean: "Looks great!". Then the census was removed.

## 2026-10-09: the flat resize loop

The game retried a refused ResizeBuffers every frame. The held back-buffer
reference was the draw-packet capture's `s.drawPacketOutput`; it is released
in flatRuntimeResize, and the loop's reports are rate-limited (771bb99a). Not
yet exercised: a borderless resolution change does not call ResizeBuffers. The
exit crash at +0x4d78c51 is tracked separately.
## 2026-10-09: device change, and the windowed SS 0.5 smear (13:23 flight, ea04a8a5)

Device change (review P2): the shared layer's device-owned caches (blend cache,
seeder and coverage deferred contexts, coverage and composite shaders, parameter
buffer, format answers) survived a device change. Fixed in eff0f8a6:
uiLayerDeviceReset on a device the flat layer has not seen; a same-device resize
rebuilds nothing; tools\ui_layer_device_test gates it on two WARP devices.

Smear at windowed 1920x1200, SS 0.5 (trained-upscale R 960x600), reached by a
resize from 4K and then the SS change; the 30 s window: tone-unproven 1005,
tone-proven 12 of 1040, composites 2, back-offs escalating to 30 s.

- ruled out: H1 (eced5813 broke copy-route proof), because fullscreen 4K SS 0.5
  is sharp on ea04a8a5.
- ruled out by code: H3 (stale identity after a resize or render-size change) as
  the cause: the proof was reset on resize and keyed by frame; a new H costs one
  unproven frame.
- Found in code and in the same log (the fullscreen SS 1.0 window before the
  resize: only holograms asked, tone-unproven 894, candidates 7-8 "its copy 0"):
  (a) the HDR-route trigger copy's source was read from the binding shadow,
  which is stale after treatHdr ran in that scope, so the copy alias was never
  recorded; (b) one target -- the frame's first ask, here a hologram -- stood for
  every HUD target; (c) a draw into a target the frame had already copied or
  tonemapped was still taken, and lost (the back-offs). Fixed (built, not
  flown): the copy source read from the context; the proof per target; a draw
  into an already consumed target refused as `after-tone`; the tone-candidate
  log re-armed for eight lines after every route, render-size or swap-chain
  change, with the route name and every HUD target and its copy.
## 2026-10-09: f86f28df flown; the loading-screen hologram ghost (log 135155)

- f86f28df flew well: the windowed mid-session change is fixed (resizes
  `result=ok refs=0`; the windowed cockpit window copies 2649, composites 2647,
  no back-offs). ruled out: H3/H2' distinction -- f86f28df fixed the
  mid-session windowed case (log 135155).
- New: the loading screen's hologram ghosts/smears (Sean: not there before the
  port). The flat layer took one of the hologram's seven pairs (94D5C556DFD6D705
  / 912477AEF6958379, the only one with camera rows); the other six stayed in the
  frame (no-camera-rows). One picture split: that part unjittered over the
  upscale, the rest jittered through DLSS, the taken part flipping in and out of
  the layer as the route treated or refused frames (13:53:25: composites 267 of
  1616 copies, door-refused-untreated 449). ruled out: (c) panel factor for the
  loading ghost, factor x1.0 throughout.
- Fix (built, not flown): a family is taken whole or not at all, so flat does
  not take the holograms family; they stay in the game's frame as before the
  port. The holo panels, the flight HUD and the target sprite are unchanged. VR
  unchanged.
- No in-frame "non-scene" signal exists before the HUD draws: the copy
  structure's verdict comes at the frame's final copy. The door already gates
  the next frame on this frame's resolve (door-refused-untreated), which is
  the cheap per-frame signal there is.
- Flown 1f27a834 (log 141012): on the SS 1.0 loading screen the layer is
  idle (holograms asked=0, tone-candidates=0, composites=0) while the HDR
  route treats every frame (treated=410 of 410 per 5 s, scene 32x32), and the
  ghost remains. With fix.ui_quality off the ghost is still there (Sean).
  ruled out: the UI port as the cause of the SS 1.0 loading-screen hologram
  ghost, because the layer is idle there and turning the key off does not
  remove it. Open, separate from this arc: the hologram ghosts through the
  HDR-route resolve at SS 1.0 and not on the copy route at SS < 1. The
  split-hologram fix stays: taking one pair of seven was a defect in its own
  right. Full captures of the loading frame expire on the 256 MiB budget
  after 3 large draws; arm a capture on the hologram pairs instead.
- 2026-10-09: the HDR-route loading-hologram ghost may share the cause of
  design-flat-temporal-aa section 106 (NVIDIA's AutoExposure on the HDR
  route, now fixed exposure 1.0); re-check it on that build.

## 2026-10-09: the setter's panel factor is held until the scene follows it (0.19.0 review, finding 3; built, not flown)

The release review found (still present on b259e6bf) that `flatFrameBoundary` made its plan from the last published scene size. The game's Supersampling setter writes
the new factor first (`moveFactorTo`, through `uiFlatPanelMove`), and a boundary that ran after the last old-size copy and before the new size's wrote the settled
old plan back over it: 3840x2160, UI 125, Supersampling 1.0 -> 0.5 was 0.800, setter 0.400, boundary 0.800, and the new size's plan then waited the 11-boundary settle
for the right factor. The review's scratch check shows the reversed factor; it did not show a mis-sized panel (that needs a panel created in the interval).
- Fix: the decision after the refusal check moved into `UiFlatPanelSettle` (ui_sizing_math.h). The setter thunk records a count and the scene's size when it moves a
  flat factor. The boundary holds that factor, writing and publishing nothing, until the scene's size differs from the recorded one, then accepts the new size's plan
  at once, without settling, and writes it only if it is not the factor already there. A second setter restarts the wait. A size that never follows ends the hold
  after `kUiPanelTransitionFrames` = 120 boundaries (a guess; nothing measured says how long Elite takes) and the plan for the size there stands. A refused frame
  restarts the settle and leaves the hold; the key off and the anti-aliasing off forget it. Plain resizes settle as before.
- Log: "the scene is now WxH, N boundary(ies) after the game's Supersampling setter moved the factor ... accepted without settling" and "the scene stayed WxH for N
  boundaries ... the plan for that size stands"; the flat 30 s line counts the boundaries held and the waits that followed or gave up. Absent means the setter never
  ran in a flat session.
- Tests: ui_quality_test `flatsettle` (ui_flat_panel_settle_test.h): the review's case, setter before the copy, unchanged dimensions, a setter that moves nothing, rapid
  changes (before the first copy, after it, between its copy and its boundary), the cap and the floor, refused frames, the key off, and source pins on the glue with
  controls. Fifteen one-edit mutants of the class and the glue, run by hand against the rig, are each caught (hold ignored, epoch ignored, arrival through the settle,
  arrival unsettled, no timeout, timeout off by one, no restart, hold publishes, forget keeps the hold, keep tolerance zero, refused frame does not unsettle, arrival
  ignores the size, and three glue edits).
- 2026-10-09, follow-up review of dbbbcf03, finding 2 (built, not flown): a setter that landed between the boundary's epoch read and its write was overwritten (epoch 0 read, setter 0.400 and epoch 1, 0.800 written back, then a hold on it). The boundary's epoch read, decision, publish and write (`uiFlatPanelBoundarySection`) and the setter's published-plan read, factor write and epoch (`uiPanelSetterSection`) are now each ONE operation under `g_floatLock`, the floats' lock; under it only atomics, `UiFlatPanelSettle::step`, the move arithmetic, the floats' write (`writeFloatsUnlocked`) and one load of the scene's size; the live Supersampling is read before it and the log written after. Tests: `testRace` injects a setter at each point of the boundary's section (including a boundary that writes 0.533), before its lock, and a boundary into the setter's; mutants that drop either lock, read the epoch outside it, or lock only the write fail them.

## 2026-10-10: the System Map's draws into the flat UI layer (built, not flown)

Census (flat draw census, flight 081437): every draw is a TRISTRIP quad into the HDR scene (R11G11B10F, 3840x2160), depth off, alpha over (SRC_ALPHA, INV_SRC_ALPHA),
VS SRVs t1/t6 from the instanced pool, CB c=@46.
- `kMapCanvas`: VS 12382D2EA45E9632, PS 855C469156AB997F, n=6. PS slot 0 is a GUI canvas (RGBA8), or on the System Map a 1024x1024 BC3. 45 a frame on the
  Galaxy Map, 10 on the System Map, 8 on the Orrery; absent in the cockpit.
- `kMapSprite`: VS C31238331D8AC3D4 with PS BD0FEB3276C8B2D7 (n=6, a 64x64 or 128x128 BC3/BC7 icon) and with PS 539A4858CE3A3477 (n=12, System Map). 172 a frame
  on the Galaxy Map, 14 on the Orrery, 5 on the System Map.
- Stays in the scene: the additive star and dust fields (41C518.., 7D26BC.., 39D858..).

The canvas VS was seen before. The 2026-09-23 VR session logged it as an always-on composite the ui depth rule left alone, as "samples a learned surface"
(docs\ui-layer-2026-09-23.md, around line 1055). The VR classifier does not name it on the HDR target (uiLayerFamilyFor's HDR branch names only the pairs it
named before), so the new families are flat-only by construction and also gated on the map being open.

What was built:
- Families: `kMapCanvas` and `kMapSprite`, appended before `kCount` (ui_layer_math.h), named "map canvas" and "map sprite", in `uiLayerFamilyTakesHdr`.
  No VR classifier returns them; the two enum loops that walk `1..kCount` print only engaged or decided families, and the arrays sized `kCount` grow with them.
- Classification: `flatUiMapFamilyOf(vs, ps, mapOpen, hdrTarget)` (flat_ui_layer_math.h) names the pairs only when a map is open and the target is the HDR class
  (flatUiTargetClass). `flatUiFamilyFor` asks it first and falls back to `flatUiFamilyOf`. `flatUiLayerTakesFamily` lists both families.
- Gate: GuiFocus is read ONCE per frame, at the frame boundary (`flatRuntimeMapFocusFrame`, flat_runtime.cpp). The map plane (focus 7) and the map families (6, 7, 8,
  `flatRuntimeMapOpenFrame`) answer from that read, so a frame's answer does not change mid-frame. The map plane's behaviour is as before; it now reads at the frame
  boundary rather than at the resolve, within the same frame.
- Draws: `FlatUiLayerDraw.mapOpen` is filled where the draw is classified. A taken draw goes through the existing begin, issue, tone re-issue and composite path,
  as the HUD families do. The issue count reads the family the decision stored, not a second classification.
- Logs: the 30 s "flat ui layer families" line gains map-canvas and map-sprite. A new 30 s line, "flat ui layer map: open-frames=N; canvas asked=A taken=T refused=R
  (reasons); sprite asked=... (reasons)", prints zeros too. The first take of each family is said once: "flat ui layer: the first map canvas taken -- frame N, vs ...".
- Tests: ui_quality_test `testFlatLayerRules` (the takes list, the names, the gate on mapOpen and on the HDR class, the mixed and unrelated pairs, `flatUiFamilyFor`,
  the VR classifier on both target kinds), the `SC3a` takes-HDR set, and the HDR decision for each family (kOver redirects).

Mechanism, not yet shown in a flight:
- Raster size. A taken draw rasterises into the layer at D x T, as the HUD families do. The canvas is a 4800x2700 texture at ui_quality 125, which is 3840x2160 x 1.25.
  So the game may already draw the canvas at the panel factor, and the layer would then resample it rather than enlarge it. Unproven: a texel check needs a flight.
- Sprites are fixed-size atlases (64 or 128 px). At 125 the layer magnifies them, where the game drew them at a fixed size. Unproven: what they look like at D x T.

Risks, in order of how likely they are to make the take a no-op:
1. The map draws may carry no camera rows, or no upstream camera owner. Then every draw refuses ("no-camera-rows" or "not-upstream") and nothing is taken. The map line
   says which refusal it was.
2. The map draws are straight-alpha over. The HUD families were measured premultiplied-over; the HDR take's straight over has not been flown.
3. Draw order. The star and dust fields stay in the scene. If the game draws stars after a canvas, they now sit under the map's canvas (the layer composites last).
   Not checked.

Ruled out: none (new arc).

Next: an Epic flat flight with all three maps opened (Galaxy, System, Orrery), at SS 1.0 and UI 125. Read the "flat ui layer map:" line in each window: the
canvas and sprite asked counts show the gate; the reasons show what refused. Then look at a star field crossing a map.

### Follow-up: the map frames' tonemap (9cc6b8ba flight, log 091943)

The flight refuted the first reading. Every map ask was refused "tone-unproven" before the shared decision (flat_ui_layer.cpp: recorded at the proof
hud step, refused at the proven check), because no proof was ever recorded for a map frame. The census (every map census: Galaxy twice, System,
Orrery) shows the cause: the map draws write the scene, and VS CFA91824129ECBBC with PS C89DD4ED362D743F reads that scene in slot 0 and writes a
3840x2160 RGBA8 target, 3-5 draws after the last map draw. That PS is in no tone list (flat_mono_frame.h tonePsHdrSlot), so flatUiLayerToneCandidate never
saw it, and no proof was recorded.
- Caller audit before the change. `toneHdrSlot` and `tonePsHdrSlot` have these callers: the resolve's tone selection (flat_mono_frame.h ~310-318,
  NoTonePass / AmbiguousTonePass), the target's tone record (flat_runtime_model.h ~533, which the resolve model reads), the HDR route's input
  (flat_runtime_model.h ~247, ~300), the draw scope's tone flag (flat_runtime.cpp ~4738, ~4760), the packet capture's representative (~4591), two
  diagnostic logs (~1636, ~5500), the UI layer's candidate branch (~5437), and tests. Registering the pair in the registry would change the resolve and
  the target record on map frames, so it is NOT registered there.
- What was built: `kFlatUiMapToneVs` / `kFlatUiMapTonePs` and `flatUiMapToneSlotOf` (flat_ui_layer_math.h, with the census comment). The flat
  runtime's candidate branch (flat_runtime.cpp, the `else if (tone || ...)` near the UI layer decision) accepts the pair for the UI layer alone, with slot 0.
  The first proof from a map frame is said once: "flat ui layer: map-frame tone proven by VS ... PS ... reading slot 0, frame N (the proof holds for frame N+1);
  said once."
- Not covered: the bloom composite CFA918 / FA01CD writes the scene and stays out. Its full hash is not in the repo, so the rig pins a neighbouring PS on the
  same VS instead.
- Next: the Epic flat flight with the three maps open, reading that line and the map line's tone-unproven count.

### Follow-up 2: the candidate read a null input (1d8d8212 flight, log 094919)

The candidate branch did run on the map tonemap (candidate lines 2685-2692: "vs CFA918 ps C89DD4, HDR slot t0 reads 0000000000000000"). Its input
was null because the runtime captured SRV slots only for registry tones and copies (flat_runtime.cpp, the capture gate before the UI block). Now
the capture also runs for the map tonemap. Its readers were audited: the copy-pair branches (flat_runtime_model.h), the menu and image copies, the
mono selector (keyed on the registry), flat_copy_structure.h, and flatContractMatches (fed only by the temporal observation, which already fills
every draw). The frame-contract hash (flat_frame_contract.h) mixes the slots into a trace header for the F10 dump, not a decision.
- The first map-tone candidate is said once, proved or not: "flat ui layer: first map-tone candidate -- frame N, input <ptr>, recorded targets
  <list or none>, match yes|no (reason), map asks this frame n; said once." The reason is flatUiToneMatchWhy (flat_ui_layer_math.h, rig-pinned).
- The map line gains tone-candidates-map and tone-matched-map (zeros print).
- No rig drives the runtime's candidate branch (it needs the D3D draw scope); the pure reason function is pinned instead.

## October 11: B390 cockpit HUD remains in temporal history

User reports HUD and menu motion smear with the latest XeSS flat branch.
Environment: Steam Odyssey flat, ONEXPLAYER 3 Intel Arc B390, Proton
Experimental, DXVK, private Mesa 26.3-devel 37bf953a58; no VR runtime or eye
size applies. Correct installed code is 0f700706, build 6ACBB8D6, linked
16:27:02 UTC. The flight is edvr_gfx_20261011_105659.log. Live flat settings
are temporal_aa=xess and ui_quality=125. Render is 960x600, output 1920x1200,
UI layer 2400x1500. SDK SR runtime is 2.0.2.0 from package v3.0.2.

At 11:01:29.328, after leaving the map, the 30-second cockpit window records
1785 copies and 1785 door-armed, zero tone candidates/proven/admitted/reissued,
zero composites, and 1785 composite-none. Holo asks 21420, flight HUD asks
30230 and target sprite asks 1719 all have zero taken. tone-unproven is 53369;
other-work, not-HDR, not-upstream, no-camera, other-shift and after-tone are
zero. XeSS evaluations return rc=0. The panel scaler separately reports target
125%, x2.5 and a widest panel of 2400 pixels.

The preceding map window has 611 proven/admitted/reissued tone candidates and
611 composites. At 11:00:44.892, structural HUD reissue identifies
43CA9F1C0AD2ACFE/E8948389387DA083 reading the taken HDR HUD at PS t1. The
steady cockpit's LDR source writer is F9CFC798F21E9AEA/E8948389387DA083. Both
VS hashes are already named tone variants, but E894 is absent from the tone
slot registry. The structural fallback needs a HUD take, while HUD take needs
previous-frame tone proof, so it cannot bootstrap this gap.

Ruled out: disabled UI quality or failed XeSS execution, because the live
INI/panel scaler select 125 and the backend repeatedly executes rc=0. Ruled
out: copy admission or camera/jitter refusal as the cockpit UI gate, because
every copy arms and the refusal counters isolate tone-unproven. Ruled out:
historical FTR5 proving F9's HDR slot, because all three F9/E894 draws have the
HDR-SRV-known flag clear; null slots mean unavailable data.

Hypotheses for one capture:

- Missing cockpit tone recognition: actual F9/E894 t1 equals a current HUD
  target or its recorded alias, with full LDR output and valid tone shape.
- A downstream pass shares that pair: all physical inputs miss HUD targets, or
  the RTV/viewport/depth and read-only tone shape reject it.
- Binding drift or an alias differs from the cached draw key: physical shader
  identities and t0..t3 typed resources disagree with the recorded identities
  or match only a copy alias. Record both without changing proof state.

The existing F10 draw-packet capture records physical bindings, but its
one-per-category selection would consume the category before this late unknown
tone; it cannot reliably target the required pair. A bounded diagnostic-only
observer instead samples these two exact VS/PS pairs only after current-frame
HUD asks. Each pair has two physical snapshots per geometry, at least 90 frames
apart, and explicit progress/never-observed counts. Physical HDR RTV
observations at HUD asks also check the cached target identity. It does not
change tone proof, candidate admission, rendering or the global tone registry.
No diagnostic config key is added. Next: build, install on the branch, and
capture one steady cockpit with UI 125 and XeSS selected.

Validation before the diagnostic flight: both changed production units compile
with MSVC and the normal absolute build.bat compiles and links the production
DLLs. The 91 flat UI CPU checks pass. The targeted UI rig reports 3794 checks
and 206 failures; every FAIL line matches a clean isolated 0f700706 run (3782
checks, 206 failures) under the same Wine prefix. The normal full build stops
at the previously reproduced run_jobs process, focus and inherited error-mode
failures. It writes no full-build receipt. Keep the diagnostic on
codex/xess-flat pending the physical binding capture and native Windows
validation. Detailed log:
/tmp/edvr-msvc-bootstrap-20261011/full_ui_tone_probe_20261011.log.

Clean diagnostic 8dad2398 (v0.19.1-16-g8dad2398) is installed locally. The
normal clean rebuild again stops only at the known Wine run_jobs gate; the
fresh flat installer payload check passes. Sanctioned install dry-run, install
and verify-only pass. The live flat INI SHA256 stays
85aa64ab9f41dcdee88e8bf0e4b7ff2abece9016fe549c30e604cbaffd7bff02 (618 bytes).
The physical cockpit/menu capture is pending. Expect source 8dad2398 in its
flight log; any following docs-only commit changes no DLL input.

### Physical capture result: 8dad2398, flight 114059

The user completed the cockpit/menu test. Sanctioned reader confirms
v0.19.1-16-g8dad2398, build 6ACBC80B, linked 17:31:55 UTC. Render is now
1720x720, display 3440x1440, SS 0.5/UI 125. This remains the flat B390, Proton
Experimental, private Mesa setup; no VR runtime or per-eye size applies. The
panel factor is x2.5. At 11:44:29.875, 1372 copies all arm the UI door, but
tone candidates/proven/admitted/reissued and composites remain zero.

F9CFC798F21E9AEA/E8948389387DA083 has physical snapshots at frames 6625 and
6715, 90 frames apart. Cached and actual shaders agree. Actual PS t1 is
resource 0x7E11A240, R11G11B10_FLOAT view/resource (26), 1720x720. The
same-frame HUD's physical RTV is that exact resource, matching its cached
target. HUD precedes tone: q1977 before q2042, then q2081 before q2145. The
output is resource 0x7E11E980, RGBA8 view (28), typeless RGBA8 resource (27),
1720x720. One full viewport has depth 0..1 and no DSV. Read-only tone shape is
true, full shape true, PS b2 272 bytes, exposure present, PS t0 a 16-cubed 3D
LUT. t2 is quarter-sized bloom; t3 is null.

43CA9F1C0AD2ACFE/E8948389387DA083 at frame 6603 has the same physical HUD t1
and LDR output, HUD q2038 before tone q2116. Structural shape is true; full
shape is false solely because VS exposure t0 is absent. The existing structural
rule permits missing exposure. This pair appears briefly at scene transition;
sustained cockpit uses F9. No recorded alias is involved; all HUD and copy
mismatch/unavailable/exhaustion counters are zero.

Ruled out: F9/E894 being an unrelated downstream pass in this cockpit, because
its actual t1 equals the actual HUD target and the output has the measured tone
shape. Ruled out: binding-cache drift or an unseen copy alias explaining the
missing proof, because both physical HUD and shader identities agree and the
input directly reads H. The missing UI tone recognition is confirmed.

Implement only these two observed UI candidate pairs with slot 1, keeping map
pairs, the global tone/HDR registry and the XeSS backend unchanged. Before
initial proof, require physical shader identities, tone structure, LDR
output/full viewport at R and direct current physical HDR HUD identity. Permit
the observed 43CA missing exposure. The new candidate reads t1 physically
through the structural observer before establishing proof, so it does not need
or widen the runtime's cached SRV capture gate. Restrict its caller to one
three-vertex fullscreen draw. Keep the diagnostic observer through the fix
flight; no new config key. Native Windows full validation remains pending.

Candidate validation: production DLLs compile and link in the normal absolute
build.bat. All 97 flat UI CPU checks pass, including pair/shape/direct-H
identity and next-frame proof bootstrap. The UI rig has 3800 checks and the
same 206 FAIL lines, in the same order, as the clean Wine baseline. The full
build still stops at unchanged run_jobs focus/process/error-mode failures, with
no receipt. Root added the captured three-vertex/one-instance/start-zero
prefilter before this final build. Next flight must show tone proof, HUD takes,
reissue and composites, and inspect both HUD and menus; a newly exposed later
refusal must be diagnosed from those counters rather than hidden by a threshold
change.

Clean candidate install: source 4b7bfec4, version v0.19.1-18-g4b7bfec4.
Production DLLs compiled and linked; the full build again stops at the baseline
Wine run_jobs gate. Fresh flat installer payload validation passes. Sanctioned
dry-run, install and verify-only pass, and the live INI remains
85aa64ab9f41dcdee88e8bf0e4b7ff2abece9016fe549c30e604cbaffd7bff02. This branch
remains separate from main. User HUD/menu motion check is pending; expect
source 4b7bfec4 in that flight, with any subsequent documentation commit
changing no production input.

### Fix verified locally: 4b7bfec4, flight 120427

The user reports "HUD and menus look stable." Sanctioned reader verifies
v0.19.1-18-g4b7bfec4, build 6ACBCE36, linked 17:58:14 UTC. XeSS remains active
and executes rc=0. Render begins 1720x720, then becomes 2236x935; output stays
3440x1440, UI 125. These imply SS 0.5 then about 0.65. The UI HDR/8-bit layers
are 4300x1800. Environment remains B390, Proton Experimental, private Mesa,
flat only; no VR runtime/eye size applies.

At 12:07:07.027, 43CA/E894 first qualifies from physical HUD t1 at frame 9167,
establishing proof for 9168. That next frame is admitted and reissued. At
12:07:07.848, F9/E894 also engages. Thus the newly recognised pair ran before
the stable result, rather than relying on a later registry variant. At
12:07:57.979, copies = door = tone candidates = proven = admitted = reissued =
composites = 1083. Declines, composite-none, binding failures and all refusal
categories are zero. Holo panels take 13018/13018, flight HUD 12525/12525,
target sprites 2/2. Later steady windows similarly complete every frame. A
transition at 12:09:28 contains two missing composites, 22 not-HDR and 22
unproven asks; the following windows return to complete coverage. The user
reports no visible issue during cockpit/menu movement.

The same run later uses F9/FEE, an existing registry variant, after render size
changes. This confirms that the renderer can select different colour passes
within one device/run. It does not by itself isolate the setting or capability
selecting E894. Any setup selecting the previously unrecognised variant could
hit the original proof gap.

Ruled out: a later hidden camera-row/jitter refusal causing the reported menu
smear, because steady HUD and panel takes have zero no-camera/other-shift
refusals and the user confirms stability. Existing hologram limitations in
other scenes remain outside this tested coverage.

Removed the temporary detailed tone probe, its pair budgets/progress and
physical-copy alias diagnostics. Preserved the physical HUD snapshot used by the
permanent guard, the exact-pair shader/shape/direct-input checks, normal UI
counters and the bounded first-qualified line. Independent review confirms the
flown guard and proof/reissue path are unchanged; snapshots still hold up to
four actual resources only for the current frame, with Present/device/ resize
cleanup. Both changed units compile; cleanup CPU checks pass 88/88, UI rig 3791
checks has the same 206 FAIL lines as the clean Wine baseline. No config key or
user feature is removed. Native Windows full validation is pending before any
main merge. The installed 4b7bfec4 remains the tested path.

Cleanup validation: the normal full absolute `build.bat` compiled and linked
both production DLLs from the frozen cleanup tree
(`v0.19.1-19-g724503cd-dirty`). It stopped at the established Wine `run_jobs.py
--self-test` failures: focus attribution to the main flow, inherited error mode
0x0 and Job Object process-tree cleanup. There is no full-build receipt and the
full build is not green. Log:
`/tmp/edvr-msvc-bootstrap-20261011/full_ui_tone_cleanup_20261011.log`. Native
Windows validation remains the branch handoff gate. The installed and flown
4b7bfec4 DLL is left in place; the cleanup removes capture overhead while
retaining the reviewed rendering checks.
