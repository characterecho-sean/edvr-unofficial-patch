# Flat UI quality with TAA DLSS and FSR

## Status

- **State: SHIPPED (2026-10-09).** Flown OK on Epic flat. Branch
  `claude/flat-display-crash-ui-smearing-f1d96a`; the census was removed in the
  commit that follows the flown build.
  - SS 1.0 at 125%, HDR route: build `eced5813`, Sean: "Looks great!". In the
    three steady cockpit windows, frames = copies = door-armed = tone-admitted =
    composites = 8046; no back-offs, no declines.
  - SS 0.5 at 100%, copy route: flown OK on `75cd495c` (every window 2700 on
    every counter, Sean: "works great"). Copy route last flown on 75cd495c;
    eced5813's flat admission change not yet flown there.
  - Not flown: 100% at SS 1.0.
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
- **Temporary config keys:** none.
- **Next:** fly SS 0.5 on the shipped build (the copy route under eced5813's
  admission change) and 100% at SS 1.0.

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