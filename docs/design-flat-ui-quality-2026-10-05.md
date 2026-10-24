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
- **2026-10-15, RC1 flat report (EDHM chained): built, not flown.** The cockpit
  tone PS 0D4329BDF65DE8F7 was in no table, so the layer never armed and the
  panels ghosted. It joins E894 in `flatUiSourceTonePair`, and (Sean) any unknown
  pair on a tone VS that passes the full physical shape now grants proof
  (`flatUiLayerShapeToneCandidate`). Entries at the end.
- **The key:** `fix.ui_quality = off | 100 | 125 | 150 | 200`, default 100 in
  flat (the code's fallback and the flat ini template), the fourth flat F8 row.
  100: the panels at display size and the cockpit HUD out of temporal history,
  whatever the render scale; 125/150/200: the same 1.25x/1.5x/2x larger (150
  and 200: 2026-10-21); off: today's frame. Both need the flat AA on.
- **What ships:** the panel factor `f = (R/D)/T` (ui_panel_scale.cpp, "ui
  quality: panels (flat)" lines); the flat UI layer (flat_ui_layer.{h,cpp}, a
  mono adapter over ui_layer.cpp) for the holo panels, flight HUD and target
  sprite, its "flat ui layer" 30 s lines and refusal counters. The holograms
  family is not taken (only one of its seven pairs has camera rows in flat;
  the 13:51 loading-screen ghost): it stays in the frame as before the port.
- **Removed:** the flat UI census (flat_ui_census.{h,cpp}); its target-class
  and family rules live on in flat_ui_layer_math.h.
- **Known leftovers:**
  - the holograms stay in the frame, through the anti-aliasing (six of seven pairs
    carry no camera rows flat reads: 9B34C331902DC1ED, DF3503CD07F9B10C,
    5453D19B6D362364, A2C2D5510BF1926D, 9611A454527F7FEB, B932058F26B76691);
  - flight HUD `other-shift`: rare, a camera shift of 2x the frame's y phase;
    left in the frame;
  - the flat resize-loop fix (771bb99a): exercised OK by windowed resizes on
    13:23 and 13:51 (`result=ok refs=0`).
- **Ruled-out pointer:** the 2026-10-09 entries; [crisp-ui-handoff.md](crisp-ui-handoff.md).
- **0.19.0 review (10-09), finding 3:** the Supersampling setter's panel factor is held until the scene's size
  follows it, its check and write one operation under the floats' lock (built, not flown).
- **System Map families (10-10, built, not flown):** `kMapCanvas` and `kMapSprite` (flat only; asked while GuiFocus 6, 7
  or 8 is open, on the HDR target) go into the flat UI layer like the HUD families, drawn at D x T after the upscale
  ("flat ui layer map:"); 10-22: the game-334473 Galaxy icon PS joins, and the Full System Scanner's tone is named too.
- **Temporary:** the back-off trail log lines (10-15) go once the camera-suite HUD loop is fixed.
- **Local flight, next locally:** 4b7bfec4 (SS 0.5 then about 0.65/UI 125) verified the rendering path; cleanup 02f50bd7 keeps
  the flown guards (88 CPU checks), no repeat flight needed. Keep codex/xess-flat separate pending the user's Windows full build.
- **2026-10-22, the scanner's zoom (second fix built, not flown):** a one-ulp camera difference between HDR writers refused the map
  recovery's frames; the copy-branch waiver missed the scanner (flight 135701), now also at the admission by structure.
- **2026-10-24, HUD Anti-Aliasing (FLOWN OK at 1440p on 88a54953, merged to main):** the HUD layer is composited after the AA, so below
  4K its slanted edges stepped. SMAA 1x over the layer before the composite is the user setting `fix.hud_antialiasing` (F8 "HUD
  Anti-Aliasing", flat Performance tab under UI quality, default off; "flat ui layer smaa:" lines). Seen working at 2560x1440 (15:43);
  nothing to do at 4K, where the edges already carry coverage ramps. Owed: the maps, and its frame-time cost. Last entry.
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

## 2026-10-15: an EDHM tone variant kept the layer from arming (RC1 flat report)

**The report.** A v0.20.0-rc.1 flat user (Steam, 1920x1080, EDHM chained
through `real_dll = d3d11_edhm.dll`; BloomQuality 1, DOFEnabled 2): cockpit
panel text ghosts under temporal AA. The right-hand Inventory panel's left
half shows doubled lines; its right half is clean. Log
edvr_gfx_20261015_125511 (build v0.20.0-rc.1, profile flat). The user cycled
every AA mode and every `ui_quality` value during the session.

**What the log shows.**
- Every 30 s cockpit window: `flat ui layer: ... tone-candidates=0
  tone-proven=0 composites=0`; families `holo-panels asked=27858 decided=0
  taken=0`; refusals `tone-unproven=33718`. The layer was armed and never
  took a draw, so the panels stayed in H and were resolved with whatever lay
  behind them. Over the moving starfield that ghosts; over static cockpit
  geometry it does not, which fits the half-ghosted panel.
- The one window that composited (91, 12:58:44-12:59:14) was the Galaxy Map,
  proven through the map tonemap pair.
- The HDR route took VS F9CFC798F21E9AEA with PS 0D4329BDF65DE8F7 as its
  trigger. Its test is structural and reads no hash.
- `flat ui layer: unknown source-tone probe -- VS 43CA9F1C0AD2ACFE PS
  0D4329BDF65DE8F7, physical HUD t1=..., LDR RTV fmt=28, bound 2D t2=...`:
  the E894 shape. Unknown pairs never bootstrap proof (the October 11
  decision), and 0D43 is neither in the tone registry nor in
  `flatUiSourceTonePair`.

So the cockpit tone never reached `flatUiLayerToneCandidate`. The registry
records an earlier EDHM grade PS (EAA5F18F10533BD1, Epic 2026-09-26), so a
different EDHM version or settings tier is the likely source of 0D43; the log
cannot say which.

Not chased: the motion the panels' pixels get inside H. Once the layer takes
the panels they leave the resolve, and that motion no longer applies to them.

**The fix (branch claude/flat-edhm-tone-panel-smear).** PS 0D4329BDF65DE8F7
joins E894 in `flatUiSourceTonePair`, with the same two VS. Admission still
runs every physical check (`sourceTonePhysicalShape`). Like E894, it stays
out of the global tone registry and the mono HDR selector.

**Flight check.** In the cockpit, the 30 s line shows `tone-candidates` and
`composites` above 0 and `holo-panels ... taken` above 0.

## 2026-10-15: an unknown tone pair proves the tone by its physical shape

**The decision (Sean, 2026-10-15).** It supersedes the October 11 rule that
unknown pairs never bootstrap proof: "let an unknown tone pair that passes the
full physical shape check grant UI-layer proof, so new EDHM versions don't need
a named pair." Each new EDHM version or settings tier can bring another tone
PS, and until now each one switched the layer off for that user, silently,
until a report named it.

**The rule (`flatUiLayerShapeToneCandidate`, branch claude/flat-ui-tone-by-shape).**
A full-screen triangle (count 3, one instance, start instance 0) whose pair is
in no table grants proof exactly as a named pair does, only when all of these
hold:
- its VS is one of the three tone VS (F9CF, 43CA, CFA9; EDHM replaces the PS);
- the named pairs' strict physical shape (`sourceTonePhysicalShape`, not the
  diagnostic one): one RTV, no DSV, no blend, PS cb2 at least 256 bytes,
  the LDR output the copy reads (view format 28) at render size and full
  viewport, the HDR read at t1, which is a physical HUD target of this frame,
  and a 3D LUT at t0;
- a 2D texture at t2 (the bloom, as the probe measured on 0D43).

Each admitted pair is named once in the log (`tone pair admitted by its physical
shape`). The 30 s line ends with `tone-by-shape=`. A pair that fails stays the
probe's near miss, as before.

**Why the shape is enough.** The draw reads the very HUD target the layer
recorded this frame, at render size, and writes the full-size output the
game's copy reads. Bloom, depth of field and exposure read H at reduced size or
write float targets, so none of them can pass.

**Flight check.** An EDHM-chained cockpit: `tone-by-shape` above 0, then
`composites` and `holo-panels ... taken` above 0, and the admitted pair named
once.

## 2026-10-15: after the camera suite the HDR HUD path backs off forever

**The report.** The same RC1 flat user, with EDHM switched off in its own tool
(the tone was the named E894, and the layer worked): "once I switch out of
camera-mode, my HUD becomes smeary". Log edvr_gfx_20261015_140119 (v0.20.0-rc.1,
flat, FSR, ui_quality 125).

**What the log shows.**
- 14:03-14:08: the layer took the HUD every frame (3,586 composites in a 30 s
  window, no refusals).
- 14:08:49-14:09:20: the camera suite, visible as six `camera-cut=1` resolve
  resets. At 14:08:53 a cut frame had no tone pass (`no-known-tone-pass` x4),
  so the taken HUD never reached a tonemap and the HDR HUD path backed off for
  30 s. That back-off was correct.
- Every re-arm after that (14:09:23, 14:09:53, 14:10:23, 14:10:53, 14:11:23)
  backed off again within 15 ms, about two frames, with the same "taken but
  never came back". The window lines read `state=off`, `copies=1`, 11 HUD asks,
  to the end of the log. Temporal AA itself treated every frame with valid
  history throughout.
- The re-arm frame is healthy: its tone candidate line reads E894, the HUD
  target at t1 and proof for the next frame. The next frame, whose HUD is taken,
  logs no tone candidate, no `crisp hud: tonemap re-issue declined` and no
  unknown-pair probe.
- The same immediate failures appeared at 14:04:10-14:04:24 while the user
  switched AA modes, and cleared on their own at 14:04:57.

**Candidates, not yet separated.** In the first frame with a taken HUD, one of:
- the tone draw fails `sourceTonePhysicalShape` (for example the direct-HUD
  identity, if it reads a copy of H);
- it passes, but `uiLayerCrispAdmitFlat` refuses it;
- the re-issue declines;
- the HUD is taken from a target no tone reads;
- the tone draw never reaches the flat runtime's UI block.

**The instrument (branch claude/flat-ui-backoff-trail, log only, temporary).**
At each back-off of the HDR HUD path it prints two `flat ui layer: back-off
trail` lines, one for the previous frame and one for the current frame. Each
gives the HUD asks, takes and refusals, and every tone-shaped draw: its branch,
the first physical check that failed, the t1 identity, proof, admission and the
re-issue outcome. At most 12 a session. Remove it once the loop is fixed.

**Flight.** Steam flat, FSR3: about a minute in the cockpit, then the camera
suite, cycling the interior cameras, then exit and another minute in the
cockpit.

## 2026-10-21: 150 and 200

A user on the bloom test build (3440x1440, DLAA, `fix.ui_quality` 125, every
HUD draw taken, none refused) still saw aliasing on the HUD. With the layer on
the HUD is out of the temporal history, so the layer's size is the only
smoothing it has: the composite is an exact box over each pixel's footprint, so
a pixel is the mean of T squared samples (1.56 at 125). Built, not flown:
`off | 100 | 125 | 150 | 200` (T = 1.5, 2.0).

- 150 is a fractional ratio, so the weights are uneven (2/3 + 1/3, and three
  texels off the grid); 200 is the exact 2 x 2 mean, four samples a pixel.
- The composite shader and its CPU model already took a footprint of up to
  three texels (`kUiLayerMaxTaps` 4, "s <= 2 needs at most 3"), so no shader
  changed. The panel factor `f = (R/D)/T` already clamps to [1/4, 1] and the
  budget holds the widest panel; the grid test now runs all four targets.
- Memory (the 8-bit layer, the HDR layer and the depth-stencil target, 20
  bytes a pixel) at 3440x1440: 99 MB at 100, 155 at 125, 223 at 150, 396 at
  200. A 4074x3938 headset door makes an 8148x7876 layer an eye at 200, over a
  gigabyte an eye: the ini says so. A layer past 16384 on a side is refused as
  before (an 8192-wide display is the last one 200 serves).
- Not changed: the holograms stay in the frame, so no size here smooths them.
- Flight: UI quality 150 and 200 in the cockpit; the `ui quality:` lines name
  the size (`150%`, `200%`) and the layer's size and MB.

## 2026-10-22: the Galaxy Map's icons left the sprite family with game build 334473 (built, not flown)

Sean: the Galaxy Map's system icons deep below the galactic plane flicker as the view moves (flat, XeSS, 960x600 to 1920x1200,
census 8 of `edvr_gfx_20261022_101031`).

- The icons are VS C31238331D8AC3D4 with PS 964221955A9E77E7: 174 a frame, n=6, BC3 64x64 / 128x128, into the 960x600
  R11G11B10F scene with depth off, stencil write only (ref 4, ALWAYS/REPLACE) and alpha over. That is the state of the
  BD0FEB32 and 539A sprites the family takes. PS BD0FEB3276C8B2D7 is in no retained log.
- Every Galaxy Map session since the game update of 2026-10-20 (332841 to 334473) reads `sprite asked=0` while the canvas is
  asked 25k to 73k times per 30 s. The 10-10 flight took 424553 of 424571 Galaxy sprites. So since the update the icons were
  never asked: they stayed in the scene at 960x600 with SMAA and EASU, under the labels, which the layer takes.
- Fix: `kFlatUiMapSpritePsC` = 964221955A9E77E7 joins `flatUiMapFamilyOf`'s sprites, under the same map-open and HDR-target
  gate. PS 96422195 also draws on the System Map; any of its draws that tests depth is still refused by the layer's
  depth-stencil rule.
- Open: why only the below-plane icons flicker. The working guess is size. The VS builds a camera-facing world-space quad with
  no screen-size term, so the farthest icons are the smallest, and quads near a pixel at 960x600 flicker on and off. Taken at
  the output size they get twice the grid. If the deepest still twitch, dump the PS (`advanced.glare_shader_dump=1`, one
  launch) for a dim scalar or dither.
- ruled out: a depth or plane z-fight on the Galaxy icons, because DepthEnable is 0 on every icon and canvas draw and no draw
  in that range tests stencil.
- ruled out: a separate below-plane icon pass, because census 8 has one 174-draw sprite pass with one PS and one state.
- ruled out: the particle billboard fix, because the log reads `particle target off (stock)` throughout the map windows.
- Flight: open the Galaxy Map and look below the plane. The 30 s `flat ui layer map:` line should read sprite asked about five
  times the canvas count (174 against 34 a frame) and taken equal to asked; `sprite asked=0` there means the pair is still
  not named.

## 2026-10-22: the Full System Scanner's text (built, not flown)

Log `edvr_gfx_20261022_112019`, build d4abc498, Steam flat on an Intel Arc B390, render 960x600 to output 1920x1200, GuiFocus 9 from
11:26:21 to 11:30:03; census 2 at 11:26:50.760 (two frames). The scanner's text was never taken by the UI layer.

- What the scanner draws. The text is the holo panels (VS 81216C77F90DEDD6, 4 a frame) and the target sprite (VS E508648660A352B2, 16
  a frame) into the 960x600 R11G11B10F scene (@45, resource 7ABC4B80). Draw #210, one triangle, VS F9CFC798F21E9AEA with PS
  6DE70EBBF82FCBE6, reads that same resource at t0 (@156) and a 480x300 R11G11B10F bloom at t1 (@176 = @168, the target of the bloom
  chain's draw #204), and writes a 960x600 format-27 target (view 28). 20F383BB/DED87960 upscales that to the 1920x1200 output.
- Why the text stayed in the scene. Every 30 s window from 11:27:20 to 11:30:00 reads `holo-panels asked=7200 decided=0 taken=0`,
  `target-sprite asked=28800 decided=0 taken=0`, `refusals: other-work=0 not-hdr-target=0 not-upstream=0 no-camera-rows=0 other-shift=0
  tone-unproven=36000 after-tone=0` and `tone-candidates=0 tone-proven=0`, with copies = door-armed = 1800. The layer takes a draw only
  when the previous frame's tone pass was seen reading that draw's target, and 6DE70EBB is in no table: not the registry
  (`tonePsHdrSlot`), not `flatUiSourceTonePair` (the E894 layout: LUT t0, HDR t1, bloom t2), not `flatUiMapToneSlotOf`. The shape
  admission (HDR at t1, a format-28 target, a 2D at t2) cannot match it either: `tone-by-shape=0`. The cockpit, same families and same
  state in the same session, is taken (the 11:26:20 window: holo asked 7953, decided 5851, taken 5851) because its tone pair is named.
- ruled out: other-work, not-hdr-target and after-tone as the scanner's blockers, because all three read 0 over 36000 asks a window and
  tone-unproven is the first test after them. ruled out: the door or the copy, because copies = door-armed = 1800 in each window.
- Fix. The pair is named in `flatUiMapToneSlotOf`'s table (flat_ui_layer_math.h, `kFlatUiScannerToneVs` / `kFlatUiScannerTonePs`), slot 0,
  exact pair only: it is the UI layer's own table, so the candidate branch (flat_runtime.cpp 6737-6749) takes it as it takes the map tone.
  It is NOT in the registry, for the reasons recorded under the 10-10 follow-up (the resolve's tone selection, the target's tone record and
  the HDR route's trigger must not see a new pass); a rig pin keeps 6DE70EBB out of flat_mono_frame.h and tonemap_admit.h. The VS 43CA9F1C
  (the transition variant E894 also has) is not named: no scanner log shows it with this PS. `flatUiMapToneScreenOf` says which screen's
  tone a pair is, so the two once-only lines name it ("Full System Scanner tone proven ...", "first Full System Scanner tone candidate
  ..."; the maps' lines are unchanged) and each screen is said once. The map line's `tone-candidates-map` / `tone-matched-map` count both.
- The bloom does not double (checked, 2026-10-20 "bloom counted twice" is the risk). The re-issue replays the tone with the HUD layer at the
  admitted HDR slot and zeroes every other scene-shaped input; it is not keyed to the E894 layout's t2. `crispFindSceneImages` scans PS
  t0..t15 of the admitted draw (ui_layer.cpp 5243) and marks each input the rule `toneInputIsSceneImage` (ui_layer_math.h 1734) calls a
  scene image: a 2D colour view of a GPU-written texture, within a pixel of the HDR source's shape, 1/32 to 1x its size, not the HDR slot.
  The HUD's steps bind null there (`crispBindStep`, ui_layer.cpp 5173-5178; `toneStepKeepsSceneImages`, weapons_fire.h 173), at Begin
  (5455-5456). The scanner's t1 (480x300, format 26, a render target, half of 960x600) passes the rule and t0 is the HDR slot, so t1 reads
  zero and the layer is T(HUD) alone. The same layout is flown: 9270C355 (t0 scene, t1 480x300 fmt 26) logs `t1 480x300 fmt 26 ZEROED`
  at 112019 line 6735 and 101031 line 36791, and the Galaxy Map's census 8 tone is that pair (t0 @120 960x600, t1 @136 480x300, both fmt
  26). The map tone C89DD4 itself is in no retained log (after game build 334473 the maps run 9270C355), so its bloom slot is unrecorded.
  The rig pins the rule on the scanner's measured inputs and that the scan covers every slot, with a control edit (ui_tone_scene_test.h).
  The 10-21 glow feed (the HUD's light joins the bloom chain's first level) is separate: it should run on the scanner's half-size first
  level (draw #198, 960x600 to 480x300) as in the cockpit, and its `flat ui layer glow:` lines say so.
- After the proof. Refusals that run after tone-unproven (flat_ui_layer.cpp 266-299) have never been exercised on the scanner. From code:
  not-upstream reads `flatCameraInjectUpstreamOwns()`, and all 44 five-second `flat camera inject` lines of the scanner read
  `owner=upstream not-upstream=0`. Jitter is zero (temporal AA is off at focus 9: `zero-phase-resolves` = frames), so a draw whose rows
  measure no shift counts as phase 0 and is taken; other-shift needs rows with an off-centre of their own. no-camera-rows needs the
  draw's VS b1 buffer to be missing from the camera table (any buffer wide enough to carry rows 270..275 that the game wrote whole is in
  it); the holo and sprite VS are the cockpit's and should bind the same scene constants, but that is expected, not observed. The shared
  decision (depth-stencil-test, blend-refused) sees the cockpit's draws and states.
- Flight (flat, UI quality on, Full System Scanner open, one 30 s window): `tone-candidates`, `tone-proven`, `tone-admitted` and
  `tone-reissued` about the frame count (1800 at 60 fps), `tone-declined=0`, `composites` about the frame count, `composite-none` near 0;
  `holo-panels asked=7200 decided=7200 taken=7200` and `target-sprite asked=28800 decided=28800 taken=28800` (less the first frame or
  two); `tone-unproven=0`; `tone-by-shape=0` (named, not admitted by shape); `tone-candidates-map` equal to `tone-candidates` with
  `open-frames=0`. Once: `first Full System Scanner tone candidate`, `Full System Scanner tone proven by VS F9CFC798F21E9AEA PS
  6DE70EBBF82FCBE6 reading slot 0`, and `crisp hud: tone vs F9CFC798F21E9AEA ps 6DE70EBBF82FCBE6, HDR source t0 960x600 ... t1 480x300 fmt
  26 ZEROED`. Failures: tone-proven above 0 with tone-admitted 0 (read the tone candidate line's re-issue reason); tone-declined above 0;
  a new `no-camera-rows`, `not-upstream`, `other-shift`, `depth-stencil-test` or `blend-refused` count; `crisp hud: ... no scene image to
  zero` or a brighter halo round the scanner's bodies than with `fix.ui_quality = off` (the bloom twice).

## 2026-10-22: the Full System Scanner's zoom (built, not flown)

Log `edvr_gfx_20261022_130004.log`, build a7a5f5ac, Steam flat, Full System Scanner (GuiFocus 9). From 13:08:27.5 the scanner zooms onto a
planet and the HUD text goes blurry for runs of about 2.4 s, crisp for about 1 s. Before the zoom the same session has no refusals.

- Evidence. On the maps and the scanner temporal AA is off (`flatUiMapTemporalOff`): no jitter, no history. The frame gets SMAA and EASU
  through the `map-temporal-off` treat block in flat_runtime.cpp, and the UI layer's door arms only on a frame whose copy was treated
  (`flatUiLayerDoorArms`), where the text is composited at output size. The zoom adds the scanner's reveal composite (VS 953C8123AD8DC13B,
  PS A473A867BDDBC342, fss-scanner.md) as a second camera-bearing writer of the scene HDR, bound to the b1 of the deferred lighting pass
  (VS 7E38A6AA1269C901, PS 7CECABDE34FFBE9E) whose witness camera is the HDR's reference. Its rows differ from the reference by one float
  ulp in a few words on about 70% of frames: `[13:08:27.509] flat runtime conflict: ... cause=hdr-camera-changed ref-vs=7E38A6AA1269C901
  ref-ps=7CECABDE34FFBE9E cur-vs=953C8123AD8DC13B cur-ps=A473A867BDDBC342 ... changed-words=270.0:BD9C2552>BD9C2551,271.0:BE76B5BF>BE76B5BE,
  272.0:BD22A498>BD22A497,272.1:3E3CB2E6>3E3CB2E5`. `flatRuntimeSameCamera` is an exact memcmp, so the target went bad and the copy was
  refused `conflicting-hdr-target-or-camera` (35-96% of the frames of each 5 s window). The treat block sits after the selection, so a
  refused frame got no spatial treatment, the door stayed unarmed and the text stayed in the scene at render size. Camera injection is off
  on the scanner (`injected=0`). UI layer refusals are only not-armed; the tone is proven.
- ruled out: tone-unproven, tone-declined, other-shift, no-camera-rows, depth-stencil-test and bind-failed as the cause of the blur, because
  all read 0 while zoomed. ruled out: a second or different tone pass, because census 1 shows one tone pair per frame. ruled out: the
  camera injector, because `injected=0` there.
- Root cause. A check that guards temporal history (a second camera on the HDR would reproject wrongly) was gating a path that has no
  history. Not loosened: `flatRuntimeSameCamera` stays an exact compare and no epsilon is added, so every temporal screen keeps its
  strictness (the cockpit, the DSS).
- Fix, first (16734035, flown as 135701: did not take). `FlatRuntimeTarget::badCauses` records every cause that made a target bad
  (`flatRuntimeNoteBad` is the one place the flag is set; the forward authority's revocation goes through it as cause None), where
  `firstBad` keeps only the first. `FlatRuntimePrefix::temporalOff` is the frame's one GuiFocus read (the same local that gates the treat
  block, assigned after the prefix is cleared; false in every replayed trace). `flatRuntimeCameraConflictSpatialOnly` holds when temporal AA
  is off, the target still carries its first camera, its layout is unchanged and CameraChange is the only cause ever marked.
  `flatRuntimeBadTargetRefuses` asks it, and notes `cameraConflictWaived` or `cameraConflictKept` on the prefix. It was asked in the
  reducer's copy branch only.
- Flight 135701 (build v0.20.2-41-g16734035, Steam flat, XeSS 960x600 to 1920x1200, scanner from 13:59:28): all 22 `flat map focus 5s:`
  lines with focus=9 read `camera-conflict-spatial=0 camera-conflict-kept=0`, while `flat runtime refusal 5s: reason=conflicting-hdr-target-or-camera`
  read 59, 195, 213, 212, 118, 157, 291 ... (16 windows from 14:00:02) with `flat runtime conflict 5s: cause=hdr-camera-changed` equal to it,
  and the 30 s windows after the zoom read `door-armed=590 door-refused-untreated=1195` (14:00:31), `719/997` and `767/974` of about 1750
  copies, against `1789/3` the window before. The conflict lines were the old ones (cur-vs 953C8123AD8DC13B, one to nine words one ulp off).
- ruled out: the copy-branch waiver alone, because flight 135701 refused at the admission by structure (flat_copy_structure.h
  `flatCopyAdmit`, 324, calling `flatSelectHdrRouteAt`, whose `if (target->hdrBad)` is flat_hdr_route.h 405) with
  `camera-conflict-spatial=0 camera-conflict-kept=0`. The scanner's tone pass (VS F9CFC798F21E9AEA, PS 6DE70EBBF82FCBE6) is in no whitelist,
  so the reducer's copy branch answers `no-known-tone-pass` at its tone check, before it ever looks at the HDR's faults (flat_runtime_model.h,
  the copy branch: `tone->tones != 1`), and the admission by structure takes the frame. The log said so and the first fix read past it:
  `flat copy structure 5s: whitelist=0 admitted=92 selector-refused=195 last=selector-refused scene=960x600 output=1920x1200 VS=F9CFC798F21E9AEA
  PS=6DE70EBBF82FCBE6` (14:00:07), `selector-refused` equal to the window's conflicting-hdr-target-or-camera refusals, and before the zoom
  `admitted=300` a window. The HDR route's trigger selection refuses the same H too (`flat untrusted H audit: route=conflicting-hdr-target-or-camera`,
  `last-selector=conflicting-hdr-target-or-camera`), which is the route's own and not what refuses the copy: R < D, so it never treats the frame.
- Fix, second. `flatSelectHdrRouteAt` takes `waiveCameraOnly` (default off) and asks `flatRuntimeBadTargetRefuses` at its bad-target test; the
  admission by structure passes it, and the route's own selection at its trigger does not, so that verdict (and the untrusted-camera lines
  that read it) is unchanged. The waived frame goes on to `flatSelectHdrFrame` with the HDR's first camera (the lighting witness): the
  reveal's camera is in no record it reads. Layout, target identity (depth), provenance, an explicit write, an unnamed mark and an uncertain
  frame still refuse on both paths, and so does any frame with temporal AA on. The checks `flatSelectHdrFrame` makes after the bad-target
  test (extent, order, sources, camera against the sources) are untouched; a waived frame that fails one of them is refused with that
  reason, not as a conflict.
- Counters, in the 5 s `flat map focus 5s:` line (zeros print), now covering both paths because both are flags on the prefix:
  `camera-conflict-spatial` is the map frames the spatial recovery treated that the camera disagreement would have refused;
  `camera-conflict-kept` is the temporal-off refusals whose first fault was the camera change and which other faults also marked (the waiver
  did not apply). Rig: flat_camera_spatial_tests.h runs each case on the copy branch (a tone pass the whitelist knows) and on the
  admission by structure (an unknown tone pass, the scene at half the output, the HDR detector watching: the scanner's frame): the lighting
  pass then a one-ulp reveal, selected with temporal AA off and refused with it on; each other fault refused; the route's trigger selection
  still refusing; the predicate term by term; the wiring scan; eleven mutants of the gate caught, among them the first fix (the admission not
  asking).
- Not knowable until flown: whether CameraProvenance or another cause also marks the target on the reveal draw (the log's `cause=` is the
  first only). If it does, `camera-conflict-kept` takes the frames and the refusal stays; that is the next thing to read.
- Flight (flat, UI quality on, Full System Scanner open and zoomed onto a body): in the zoomed windows `flat copy structure 5s:`
  `admitted` about `copies` and `selector-refused` near 0 (135701: 195 and 213 of about 300 in the windows at 14:00:07 and 14:00:12); `flat runtime refusal 5s: reason=
  conflicting-hdr-target-or-camera` near 0; `camera-conflict-spatial` about the old refusal count; `camera-conflict-kept` 0. In the 30 s
  `flat ui layer:` line `door-armed` about `copies` and `door-refused-untreated` near 0; the 960x600/1920x1200 composite flips stop.
  Still reading `conflicting-hdr-target-or-camera` and fine: `last-selector=` and `flat untrusted H audit` (the HDR route's, not the
  copy's). Before the zoom both counters read 0 as before. Failures: refusals unchanged with both counters 0 (the prefix was never told
  temporal AA is off, or a third site refuses: read `flat copy structure 5s: last=` and the refusal reason); `camera-conflict-kept` above 0
  (a second cause marks the target: read the `flat runtime conflict` line); a refusal `inconsistent-draw-order` or another new reason
  (the waived frame failed a later check of the selection: the reveal composite drawn after the first consumer); a cockpit or DSS window
  with either counter above 0 (the gate leaked).

## 2026-10-24: edge smoothing for the HUD layer (built, not flown)

A user below 4K reported hard stair steps on the HUD's slanted lines. The clearest is the chamfered bracket round "RST" in the power
distributor panel: its diagonal runs show a step every pixel or two, where the game's own build smoothed them.

- Cause. The layer is composited over the finished picture, after the anti-aliasing (the game's SMAA or DLSS, or EDVR's own), so none of it
  reaches the HUD. The only smoothing a layer pixel has is the box over its footprint at T (the 2026-10-21 entry: T squared samples a pixel,
  four at 200). On a display below 4K that is too little for a diagonal run to look smooth.
- Two places the steps can come from. H1: vector geometry rasterised into the layer, which a live switch from 100 to 200 smooths at once (the
  next frame draws it into a layer twice the size). H2: edges baked into a Scaleform panel texture at the size the game laid the panel out at,
  which smooths only after a trip through the main menu has the game lay the panel out again at 200. SMAA on the layer's image works on
  whatever the layer holds, so it covers both. If a flight shows the steps surviving the switch to 200 until the menu trip, that is H2 and the
  layer's size alone never reaches it.
- Where. The flat adapter reads the key (built as a hidden developer key, default on; it is `fix.hud_antialiasing`, default off, since the entry
  at the end) on every output copy and tells the layer (`uiLayerFlatSetEdgeSmoothing`) just
  before `uiLayerComposite`. The layer takes the ask on entry and clears it, so the VR profiles, which never ask, composite exactly as before.
  With the ask, `compositeInner` runs SMAA 1x over the 8-bit layer into an R8G8B8A8 texture of the layer's size and binds that where the
  composite reads the layer. The composite shader is unchanged, and the multiply transmittance, when there is one, is read as before. Key off,
  or any failure: the raw layer, as it was.
- The passes (`ui_layer_smaa.h`, bytecode built offline with the map's SMAA). The layer holds premultiplied light in rgb and transmittance in
  alpha, cleared to (0, 0, 0, 1). A dark opaque backing is rgb 0 and a 0 against the clear's rgb 0 and a 1, so only alpha shows that edge. The
  edge pass is therefore new (`uiSmaaEdges`, `ui_layer_smaa_shader.h`): SMAA's colour edge test with the colour distance taken over all four
  channels, the threshold, steps and local contrast adaptation as `SMAAColorEdgeDetectionPS` has them. The weights and the blend are the map's
  plain SMAA entries (`smaaWeights`, `smaaBlend`) over `SMAA.hlsl` as upstream wrote it, `SMAA_PRESET_HIGH`, AreaTex and SearchTex from
  `map_smaa.cpp`. The blend writes all four channels with no colour transform: mixing premultiplied rgb and transmittance linearly is what the
  composite's own box filter already does to them, so it is exact for this composite.
- Resources and failure. Made at the first composite that asks, at the layer's size, and made again when the size moves; the compute slots the
  passes use are put back (the two samplers by the passes, the rest by `compositeInner`). A creation that fails logs once per size with the
  HRESULT and stands down at that size, with no retry per frame. With the key off the targets are let go; they also go with the layer (a
  resize, a device change, the layer not live).
- Memory. 10 bytes a layer pixel (edges 2, weights 4, output 4): 37 MB at 2560x1440 x 100, 83 MB at 3840x2160 x 100, 332 MB at 3840x2160 x 200,
  on top of the 8-bit layer, the HDR layer and the depth-stencil target of the 2026-10-21 entry.
- Cost. Not measured. Three compute passes at the layer's size, run inside the span the GPU census times as the layer composite
  (`DoorUiLayerComposite`), so that figure includes them while the key is on. The 30 s flat report times nothing of its own.
- Not changed. The holograms stay in the frame, through the anti-aliasing, so this smooths nothing of them.
- Tests (`flat_mono_resolve_test`, unit `uiLayerSmaaGpuTests`, shard 3; the build's bytecode on WARP over a 64x64 layer in the layer's own
  convention). A staircase of rise 1 over run 4 in opaque white takes 92 intermediate pixels in rgb and in alpha, with its summed opacity at
  2591.83 against 2592.00. The same staircase in dark opaque takes 92 intermediate alpha values with rgb still 0, and the map's colour-only edge
  pass in the edge pass's place leaves it a staircase (the control). An all-clear layer comes out exactly (0, 0, 0, 1); straight bands across
  the layer, horizontal and vertical, white and dark, come out unchanged; the samplers come back and no view is left bound; a size change
  re-makes the targets; a failure at a size is told once and not retried there. `flat_weapon_admission_test` runs the whole adapter and layer on
  WARP and holds the result: smoothed by default, raw with the key off, smoothed again with it on (with the default now off: raw, then smoothed with the
  key on, raw with it off again; the entry at the end).
- Log. Once per creation: `flat ui layer smaa: ready at 2560x1440 -- edges R8G8, weights and output R8G8B8A8, 36.9 MB`. Once per distinct
  failure: `flat ui layer smaa: stood down at WxH -- <what failed> hr=0x%08X`. Every 30 s, whenever the window composited the layer:
  `flat ui layer smaa: key=on smoothed=N raw-key-off=M raw-fault=K size=WxH`. `smoothed` counts composites that read the SMAA output,
  `raw-key-off` those with the key off, `raw-fault` those that fell back; `key=` is the value the adapter read, so a key the flat allow-list
  lacked would show as `key=off`. A window with composites and all three counts 0 means the composite never reached the smoothing.
- Flight. Flat, a display below 4K (2560x1440 is the case in hand), UI quality 100, in the cockpit. Look at the "RST" bracket in the power
  distributor and the other diagonal edges, then change the key to off and on again live (F8, or edvr.ini with its hot reload) and
  compare both the bracket and the small labels and numbers: the smoothing must not soften the text. Then once at 200, to see whether anything
  stays stepped (H2: switch, look, go through the main menu, look again). Then open the Galaxy Map and the System Map, whose canvas and
  icons go through the same layer. In the log: one `ready` line at the layer's size; 30 s lines with `key=on smoothed` above 0 and `raw-fault=0`,
  and after the toggle `key=off raw-key-off` above 0; no `stood down`. Failures: `smoothed=0` with `key=on` and `raw-fault` above 0 (read the
  `stood down` line and its HRESULT); `key=off` with the key unset (the flat allow-list entry is missing); text visibly softer with the key on.
  Read the census's layer composite figure with the key on and off for the cost.

## 2026-10-24: HUD Anti-Aliasing becomes an F8 setting

- Decision (Sean, 2026-10-24). The A/B key is a user setting: `fix.hud_antialiasing = on | off`, default off, live, shown in the flat F8 panel as
  "HUD Anti-Aliasing" on the Performance tab directly under UI quality (Sharp weapons now sits under it). It replaces `advanced.hud_edge_smoothing`
  completely: that key never shipped in a release, so there is no migration and no fallback read. Off by default.
- What changed. `flat_ui_layer.cpp` reads `fix.hud_antialiasing` (default false) before each composite; `flat_profile_keys.h` allow-lists it (unlisted, the
  flat gate would read it off and the row could never turn the smoothing on); `edvr.ini` has a people-facing paragraph after `ui_quality` and the flat
  sentence of the UI quality paragraph points at it; `menu_flat_rows.h` lists the row (seven rows now: with a blank line and the ten-line warning, the
  card's eighteen lines exactly, which `menu.cpp` static_asserts). The row dims, and refuses a step, while `fix.ui_quality` is off or the flat
  anti-aliasing is off, because the layer it smooths is live only with both on (`hudAntiAliasingRowDim`; UI quality is read as the layer reads it, the
  mode as the Sharp weapons row's flat branch does), and its hint and refusal name the cause that applies. The 30 s `flat ui layer smaa:` line keeps its form.
- Flat only in the menu. A [fix] row used to reach VR's Performance page by carrying `menu performance`. The new page token `menu flat_performance`
  (`tools/gen_settings_schema.py`) names a page VR's menu builds none of, as `visual` and `debug` do, while the flat Performance tab is built from
  `kFlatPageRows` whatever the token says. So the row is on the flat tab and not on VR's page. The generator refuses the token on a key that is not in
  [fix] and on a key `kFlatPageRows` does not list (the row would be on no menu at all); its self-test covers both and the misspelt token.
  Not done: the installer's settings window, VR selection, still lists every [fix] setting, this one included (the ini paragraph says it changes nothing in VR).
- Tests. `flat_weapon_admission_test`: raw by default, smoothed with the key on, raw with it off, smoothed again. `flat_sharpen_test`: the row in
  the table and its order, the gate, the dimming truth table and its pins in `menu.cpp`, the generated row on the page `flat_performance` directly under UI
  quality, the flat tab ignoring the page token and VR's page being the `performance` rows. `sharp_weapons_test`: the table order, and Sharp weapons directly
  under UI quality on VR's page. The GPU unit tests (`uiLayerSmaaGpuTests`) drive the pass directly and needed no change.
- First look (not a verdict). The 14:37 flight of a38597ab (log `edvr_gfx_20261024_143340.log`, build matched), key on: every composite smoothed,
  `raw-fault=0`. The layer was 3200x1800, so UI quality 125 and not 100. In Sean's screenshot, received downscaled to 2000x1125, the RST bracket and
  the shallow slanted panel lines showed no steps and the small text was not softened.
- Still owed. An off/on comparison at UI quality 100 at full resolution (2560x1440 is the case in hand), then the 200 switch and the main-menu trip (H1
  against H2 above), the Galaxy Map and the System Map, and the layer composite's frame-time cost with the key on and off.

## 2026-10-24: the 15:35 flight of 88a54953 (4K): no change, and why

- Flight. Log `edvr_gfx_20261024_153506.log`, build matched (`v0.21.0-25-g88a54953`), Steam, flat profile. The display was 3840x2160, not the
  2560x1440 of the report: `display 3840x2160`, UI quality 125 -> 100 at 15:36:00, layer 3840x2160 (1:1). Sean toggled HUD Anti-Aliasing about
  fifteen times between 15:37:12 and 15:39:28: "Not seeing any change to the lines around RST".
- The toggle reached the composite. Every switch logged `menu: fix.hud_antialiasing ... (written to edvr-flat.ini; live)`, every switch on was followed
  10 ms later by `ready at 3840x2160 ... 82.9 MB`, and the 30 s lines split by it (`smoothed=720 raw-key-off=1979`, `smoothed=0 raw-key-off=2700`,
  `smoothed=1107 raw-key-off=1590`), `raw-fault=0` throughout.
- What the pixels hold. Sean's full-size crop of the bracket (176x128, red channel per pixel, `scratchpad\line_profile.py`): the top line rises about
  one pixel every 8.5 px, and each row's edge is a ramp from full to zero over six to eight pixels (`8888887765321`), its half-way point moving a steady
  8.5 px per row (x = 62.5, 71, 79.5 in rows 34 to 36). That is a fully anti-aliased edge at this slope. SMAA acts on hard jumps between neighbours
  (threshold 0.1); these ramps step by about 0.1 a pixel, so it finds little or nothing, and on and off look the same.
- ruled out: the toggle not reaching the layer, because the 30 s lines split smoothed / raw-key-off with each switch and `raw-fault=0`.
- ruled out (at 4K, UI quality 100): hard pixel steps in the RST bracket, because its edges in the composited frame carry six-to-eight-pixel coverage ramps.
- Open. Whether the bracket has hard steps at 2560x1440 (the reported case), where the same artwork covers fewer pixels; the first look at 1440p (14:37)
  was at UI 125 with the key on and a downscaled screenshot, so it settles nothing. Next: 2560x1440, UI quality 100, full-size crops of the RST bracket
  with the key off and on at the same spot, read with the same profile. Hard jumps off and ramps on: the setting works where it is needed and has nothing
  to do at 4K. Ramps with it off too: no pixel aliasing for it to fix in this HUD, and the setting should be proposed for removal.
- Verdict at 2560x1440 (same log, 15:43). UI quality 200 -> 150 -> 125 -> 100, display and layer 2560x1440, HUD Anti-Aliasing toggled fourteen times
  at 15:43:13-15:43:34; the window reads `key=on smoothed=1862 raw-key-off=837 raw-fault=0 size=2560x1440`. Sean: "Okay I see it now at 1440p, ship
  it." So the setting smooths the steps where the HUD has them (below 4K) and has nothing to do at 4K, where the artwork already covers enough pixels
  to carry its own edge ramps. Merged to main, default off. Still owed: the Galaxy and System Maps with it on, and its frame-time cost.
