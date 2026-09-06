# Upscaling and foveated rendering: a design

*A design document, written before the code. Claims about EDVR cite the
source; claims about runtimes, drivers and SDKs are labelled measured
(established in this repo's field logs or code), vendor-stated (their
documentation or release notes), or believed; what can only be settled at
implementation time or in a live session is collected under Phase 0.
Nothing here is implemented yet.*

*Status, 2026-09-04. Feature 1's render scale exists on a branch; the
sharpening half shipped in 0.14.0 as `render_sharpness`, alongside the
temporal pass and NVIDIA's DLAA and DLSS at the door
([anti-aliasing.md](anti-aliasing.md)). That work measured the one number
this document lacked: NVIDIA's pass costs by output size, 2.7 ms per eye on
a Pimax Crystal Super at HMD Quality 1.0, and it made a third foveation
saving worth designing — feature 6 below, DLSS where you look. The field
rig now includes that headset, whose eye tracker runs at 120 Hz, so
feature 3's open question can be asked of a real driver; the gaze probe
(`advanced.gaze_probe`, `src/openvr/gaze_probe.cpp`) is the first code
from this document, and the phasing at the end now puts three probes
before any feature.*

## The ask

Port the performance features of OpenXR Toolkit into EDVR, so that
SteamVR/OpenVR players get them — including NVIDIA foveated rendering, and
an eye-tracked centre on headsets that can provide one.

OpenXR Toolkit is an OpenXR API layer, so for Elite it only ever worked
through OpenComposite: the game speaks OpenVR, OpenComposite translates to
OpenXR, and the toolkit sits inside that translation. Players on real
SteamVR — lighthouse headsets, Steam Link, Virtual Desktop's SteamVR mode —
were never able to use it, and the OpenVR-native alternatives that existed
(fholger's `openvr_fsr` and `vrperfkit`) are archived, and occupy the same
DLL slots EDVR occupies, so they cannot even be stacked alongside it. The
population this design serves is exactly the one nothing serves today.

The toolkit itself was discontinued in 2024. Its licence is MIT, so where
its code says something worth transcribing, it can be transcribed with its
notice — the same rule `src/d3d11/fsr/README.md` states for AMD's files.

## What translates, and what does not

The toolkit's performance page, feature by feature:

| Feature | Verdict |
|---|---|
| Upscaling (FSR) + sharpening | **Port.** Feature 1 below. |
| Sharpen-only (CAS mode) | **Port**, as a degenerate case of feature 1. |
| Fixed foveated rendering | **Port**, NVIDIA-only on D3D11. Feature 2. |
| Eye-tracked foveation | **Port**, via OpenVR's own new gaze API. Feature 3. |
| In-headset settings menu | **Port, reimagined.** Feature 4 below — the toolkit's one indispensable piece of UX, rebuilt for a cockpit. |
| Turbo mode | **No.** It defeats `xrWaitFrame` throttling, which is an OpenXR-runtime behaviour (WMR's, mostly). SteamVR paces through `WaitGetPoses`' running start and does not throttle that way; its own per-app settings already expose what is tunable. Re-implementing pacing is high regression risk for nothing measurable. |
| Frame-rate lock / motion-reprojection lock | **No.** The reprojection half is WMR-specific; SteamVR's motion smoothing already halves adaptively. |
| Metrics overlay | **Port, re-anchored.** Feature 5 — an instrument you place in your cockpit, not a HUD that rides your head. An earlier draft of this document declined a persistent overlay outright; the objection was to head-locking, and the anchoring model below removes it. |
| NIS (NVIDIA Image Scaling) | **No.** It exists in the toolkit as a preference alongside FSR; FSR is already vendored here and does the same job. A second upscaler is surface without capability. |

Two adjacent things that come up in every discussion of foveation, and why
neither is available to this game:

- **Quad-views rendering** (how DCS and MSFS do eye-tracked foveation) needs
  the *game* to render four views. It is an OpenXR concept with no OpenVR
  equivalent, and Elite will never implement it. This is exactly why the
  VRS route below — which needs nothing from the game — is the right one
  for an OpenVR title.
- **NVIDIA VRSS/VRSS2** (driver-level foveated supersampling) requires
  forward rendering with MSAA and a per-title driver whitelist
  (vendor-stated). Elite's renderer is deferred, which disqualifies it
  regardless of the whitelist.

## What EDVR already owns

The reason this port is tractable is that every interception point it needs
is already built, field-hardened, and in daily use:

- **The resolution answer.** `hookedGetRecommendedRenderTargetSize`
  (`src/openvr/system_hook.cpp`) already multiplies the size handed to the
  game — the cull guard's stage 1 inflates it, and the game demonstrably
  treats the change as an ordinary supersampling event and rebuilds its
  targets (measured; it is the mechanism the guard's two-stage go-live is
  built on). Render scale is the same lie with a factor below 1.
- **Submit-side substitution.** `hookedSubmit`
  (`src/openvr/compositor_hook.cpp`) already swaps the game's texture for
  an EDVR-rendered one on three paths (theater, eye heal, guard crop), and
  `applyCullGuard` shows the discipline: one lambda applied identically to
  every forwarding path, so no path can ship an untreated frame.
- **FSR, vendored and running.** `src/d3d11/fsr/` carries AMD's unmodified
  EASU and RCAS; `intro_upscale.cpp` computes their constants
  (`FsrEasuCon`, `FsrRcasCon`) and runs both as compute passes today, with
  the HLSL generated from the vendored files at build time.
- **The cross-DLL renderer pattern.** The openvr half calls renderers the
  d3d11 half exports (`edvrFssTheater`, `edvrFssHealLeft`), resolved by
  `GetProcAddress`, standing down loudly on a mismatched pair.
- **The eye-target classifier.** The vScreen census
  (`src/d3d11/vscreen.cpp`) hooks `OMSetRenderTargets`, `RSSetViewports`,
  `ClearState` and every draw variant, and resolves per bind whether the
  bound target is an eye texture (`targetIsEyeSized`, the `rtv0Eye` cache)
  — fed the *real* per-eye size from the submit side rather than guessing.
  This is the hard part of any VRS integration, and it already exists.
- **The projection truth.** The system hook captures each eye's real,
  asymmetric tangents, and the lied tangents when the guard is live. Both
  matter below: the fovea's position in the image is a function of exactly
  these numbers.
- **Single-threaded rendering, measured.** Submit and Present run on the
  same thread (field, 2026-08-15 — the gate note in `compositor_hook.cpp`),
  and `ExecuteCommandList` has never been seen on this game
  (`vscreen.cpp`), so touching the immediate context from the submit hook
  is safe and there is no deferred-context state tracking to build.
- **The choreography.** Two-stage go-live, live re-stage on a config
  change, per-headset gating by FOV signature, and the stand-down pattern
  (`systemHookGuardStandDown`) all transfer unchanged.

## Feature 1 — render scale, upscaled back by FSR

**What it is.** The game is told the headset wants `render_scale` × fewer
pixels per axis; it renders smaller, cheaper targets. At submit, EDVR runs
EASU to reconstruct the image at the session's native size, RCAS to
sharpen it, and forwards that — an ordinary full-bounds submission at the
size the runtime expected all along. The runtime never learns anything
changed; the compositor's own bilinear upscale (what you get from just
lowering the SteamVR slider) is replaced by edge-adaptive reconstruction
plus sharpening, which is the whole reason 75–85% scales stay presentable.

**Guidance the setting ships with:** keep the SteamVR resolution slider and
Elite's HMD Image Quality where you normally run them, and come down with
`render_scale` — the win over those sliders *is* the reconstruction, which
only happens when EDVR does the scaling.

**Mechanism, in the cull guard's own shape:**

1. Stage 1: the size answer shrinks by the factor. The game rebuilds its
   targets smaller — to it, the same event as the runtime moving the
   render resolution, which the session already handles (the
   `noteEyeTextureSize` machinery re-reads and re-publishes on exactly
   this).
2. Adoption: when both eyes submit at the shrunken size (the guard's
   changed-size test, reused), the upscale pass arms. Until then every
   frame forwards untouched — the transport never sees a shape the
   session has not served.
3. Live: at each submit, the d3d11 half's exported pass (working name
   `edvrUpscaleEye`) runs EASU from the game's texture into an EDVR-owned
   native-size texture, RCAS in place, and the submit forwards the result.
   Same export-and-stand-down contract as the theater.

**Sharpen-only mode falls out for free:** `render_scale = 1.0` with
`render_sharpness > 0` skips EASU and runs RCAS alone at native size —
the toolkit's CAS mode, at unchanged cost.

**Interactions, each decided here:**

- **Cull guard.** The two features are the same two hooks pointed in
  opposite directions, and they compose: the size factors multiply at the
  answer (guard wants margin pixels, scale removes visible-field pixels),
  and at submit the crop runs first, the upscale second. End state is a
  fused pass — EASU's constants take an input sub-viewport
  (`FsrEasuConOffset`, vendor-stated), so crop+upscale is one dispatch —
  but v1 may ship sequential (crop copy, then upscale) because the crop
  copy already exists and fusion has an edge-tap subtlety (Phase 0).
- **Transition flash.** The resubmit shadow keeps holding the *game's*
  texture — the full, small frame — and a withhold's handover goes
  through the upscale like any forward. Same reasoning as the guard: the
  shadow stores source, treatment is applied at the door, once, for every
  path.
- **Theater and heal.** Both already substitute EDVR-drawn textures built
  from game content; whatever texture a path is about to forward is what
  the upscale treats. One lambda, every path — the `applyCullGuard`
  discipline, extended.
- **The eye-size channel.** `noteEyeTextureSize` reads the texture the
  *game* submits, before any substitution, so the d3d11 half keeps
  matching the game's real (small) render targets with no change. The
  0.7.3 lesson — publish what is real, not what is guessed — already
  covers this feature.
- **An openvr-only install** (no EDVR d3d11.dll) has nobody to run the
  pass. The feature then refuses to shrink at all — a small render
  bilinearly upscaled by the compositor is a quality regression this
  project would be silently causing — and says so once, in the theater's
  "mismatched pair?" voice.

**Formats.** The pass refuses what `guardCropCopy` refuses (MSAA, arrays,
mips) and creates typed SRV/UAV views over the formats the field actually
submits; sRGB variants are a Phase 0 measurement, not an assumption.

**Settings sketch** (final names at implementation, `[fix]`):

```
render_scale     = 1.0    ; per-axis, 0.5–1.0; 1.0 = off. Restages live.
render_sharpness = 0.0    ; 0–1 RCAS strength; live. >0 alone = sharpen-only.
```

Pixel cost quoted in the log as factor²: `render_scale = 0.8` renders ~64%
of the pixels. The pass's own GPU price is measured by timestamp query and
printed alongside — believed a fraction of a millisecond per eye at
headset sizes, from the intro upscaler's behaviour; measured properly in
Phase 0.

## Feature 2 — fixed foveation, by variable-rate shading

**The hardware truth first, stated plainly:** Elite is D3D11, and on D3D11
variable-rate shading is only reachable through NVAPI, which means
**NVIDIA Turing or newer** (RTX 20-series / GTX 16-series up). AMD and
Intel expose VRS only in D3D12; there is no D3D11 path for them to expose
(vendor-stated, and it is the same limitation OpenXR Toolkit has on D3D11
titles). This feature helps a large slice of the player base and is
honest about which slice.

**What it is.** A shading-rate image — one byte per 16×16-pixel tile
(vendor-stated Turing granularity, confirmed at init) — divides each eye
into rings: full rate in the centre, one shade per 2×2 pixels further
out, one per 4×4 at the edge. The pixels still exist at full resolution;
they are shaded in coarser groups, which is where deferred lighting spends
its time. Field experience with vrperfkit's identical mechanism in this
game: roughly 10–30% GPU depending on preset and scene, planetary
surfaces the big winner. It does nothing for CPU-bound settlement frames,
and the docs will say so.

**Where it hooks.** In the `OMSetRenderTargets` thunk the census already
owns: when the bound target classifies as an eye-scene target
(`targetIsEyeSized` — the existing verdict, not a new heuristic), set the
shading-rate view and per-viewport rates
(`NvAPI_D3D11_RSSetShadingRateResourceView`,
`NvAPI_D3D11_RSSetViewportsPixelShadingRates`, vendor-stated names);
when anything else binds, clear them. The `ClearState` thunk clears
EDVR's record of what is set. No deferred contexts to chase (measured,
above).

**The fovea is not the texture centre.** In an asymmetric frustum the
straight-ahead point sits at tangent (0,0), which lands off-centre in the
image — on a Quest 3 the horizontal frustum is 54°/40° (measured, the
terrain investigation), so centring the rings on the texture would put
full resolution in the wrong place and coarse tiles at the fovea. The
rings are computed from each eye's own tangents, which the system hook
already captures — and from the *lied* tangents when the guard is live,
because the mask lives in rendered-image space.

**The guard synergy.** When the cull guard is live, its overscan margin is
rendered only to satisfy the game's culler and cropped before submission —
no one ever sees those pixels. The mask shades that whole band at the
coarsest rate whenever the guard is live, reclaiming most of the guard's
measured ~6% even with foveation otherwise off. Not rate-zero (the rates
include a cull), deliberately: post passes sample neighbourhoods, and a
black margin would bleed into the visible edge through bloom; a coarsely
shaded margin holds plausible content. The band comes from
`cropFractions`, which already exists.

**Risks, and their shape.** Coarse shading of deferred lighting and post
shows as peripheral shimmer or blocky bloom — visible and tunable, never
corrupting. Mitigations in order: conservative default rings; rates per
preset; and if the field names a specific pass that gets mauled, the
census can identify its draws for exclusion the same way it identifies
everything else. This is the feature's whole risk budget: there is no
failure mode here that outlives a settings change or a restart.

**Fail-safes.** Arming requires NVAPI to initialise, the rate view to
create, and the first set call to return OK; any other answer is one log
line — "foveation: unsupported here (this needs an NVIDIA RTX/GTX-16 GPU
or newer)" — and off. An NVAPI error after arming clears all VRS state
and stands down loudly for the session, the guard's pattern. NVAPI is
calls into the driver: nothing here touches game code or memory, and the
headers are vendored under NVIDIA's redistribution licence with the
licence text travelling alongside, as `fsr/README.md` does for AMD.

**Settings sketch** (`[fix]`):

```
foveation          = off   ; off | quality | balanced | performance | custom
foveation_headsets =       ; same FOV-signature gate as cull_guard_headsets
; custom: ring boundaries in degrees of visual angle, per-ring rates
```

**Reference: how OpenXR Toolkit does it (read from its source, 2026-09-05).**
mbucchia's OpenXR Toolkit (retired; last commit 2023-11-05) implements fixed
and eye-tracked foveation as variable-rate shading with a shading-rate image,
which is what this feature is. What its source settles:

- *Mechanism.* D3D11 is NVIDIA-only through NvAPI: capabilities via
  `NvAPI_D3D1x_GetGraphicsCapabilities` (`bVariablePixelRateShadingSupported`),
  an R8_UINT mask texture at one texel per 16x16-pixel tile
  (`NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH`) wrapped in a shading-rate resource
  view (`NvAPI_D3D11_CreateShadingRateResourceView`), a per-viewport lookup
  table from mask value to rate set on all sixteen viewports at once
  (`NvAPI_D3D11_RSSetViewportsPixelShadingRates`: X16..X1 per pixel, 2x1, 1x2,
  2x2, 4x2, 2x4, 4x4, X0 cull), and the view bound with
  `NvAPI_D3D11_RSSetShadingRateResourceView`. D3D12 uses Tier 2
  `RSSetShadingRateImage` with MAX combiners so the coarsest source wins. The
  NvAPI views are leaked on purpose and `NvAPI_Unload` is never called: both
  crashed.
- *The mask.* A compute shader writes up to four nested ellipses in NDC around
  a centre: rate 1 inside ring 1, rate 2 between rings 1 and 2, rate 3 beyond
  (ring 3 is "large enough"). Radii are PERCENT OF THE NDC HALF-HEIGHT, not
  degrees, the horizontal semi-axis scaled by "Horizontal scale" (default
  125%); presets Wide 55/80, Balanced 50/60, Narrow 30/55; rates Performance =
  1x / 2x2 / 4x4 and Quality = 1x / 2x1 / 2x2; "Prefer resolution" swaps 2x1
  for 1x2 and 4x2 for 2x4 (the default keeps vertical resolution); "Left/Right
  bias" coarsens one eye by N steps. On the Crystal Super's frustum (tan
  +-1.27 vertically) Wide's inner ring is a 35-degree radius: full shading over
  the central ~70 degrees, 2x2 out to ~90, 4x4 beyond. Its degradation starts
  far out and is gentle, and only the shading is coarse (rasterisation, depth
  and edges stay per-pixel), which is why nobody sees a disc.
- *The centre.* Not the texture centre: the head's forward axis projected
  through each eye's FOV and pose (`GetProjectedGaze`, so asymmetric and canted
  frusta are handled), then shifted 4% of the NDC half-width TOWARD THE NOSE
  per eye, "determined experimentally". That is vergence: the eyes converge on
  a cockpit at ~0.7 m and their fixation lands ~3 degrees nasal of straight
  ahead. Feature 6's disc sits on the straight-ahead point with no such term.
- *Where it applies.* The immediate context's vtable is detoured (Detours) at
  `OMSetRenderTargets` and `OMSetRenderTargetsAndUnorderedAccessViews`; every
  bind inside a frame is a candidate when the target is a Texture2D whose
  aspect matches the eye render aspect (or half its width does: double-wide),
  its width is at least 51% of the frame's dominant render width (a filter
  that follows dynamic resolution and excludes half-size effect buffers), and
  its array size is at most 2; a bind of anything else, or of null, disables
  VRS, and so does the end of the frame. There is no depth test: eye-sized
  post-processing passes are masked too, the source of its blocky bloom in
  some titles. Masks exist per distinct target size, made on demand (deferred a
  frame), aged out after 100 unused frames, regenerated only when settings
  change -- or every frame under eye tracking.
- *Which eye.* A frame analyzer: a swapchain image bound directly (forward
  rendering), a copy INTO a swapchain image (deferred: the next target is the
  other eye), or swapchain acquire order (fallback). Under OpenComposite it is
  disabled and one centred "generic" mask serves both eyes, without the nasal
  offset.
- *Eye tracking.* `XR_EXT_eye_gaze_interaction`, `XR_FB_eye_tracking_social`
  (the two eyes' poses slerped), HP Omnicept, and Pimax's aSeeVR/Droolon SDK
  (the 5K/8K module, not the Crystal's Tobii). The gaze becomes a point at
  "Eye projection distance" (default 2 m) along the ray in view space,
  projected into each eye's NDC -- vergence by construction -- and the tiny
  mask is recomputed each frame.
- *A likely dead feature.* "Cull outer mask (HAM)" stamps the hidden-area mesh
  with the cull value before the ring pass, but the ring pass writes
  `min(existing, rate)` (there so Unity's upside-down mirrored pattern keeps
  the finer rate) and every ring rate is numerically below cull, so the stamp
  is overwritten. Unverified in the field; not to be copied.

What this feature takes from it: the D3D11 recipe (the table on all
viewports, a 16x16-tile R8_UINT mask, set at target bind, cleared at any other
bind), 2x2 as the working middle rate, the per-eye nasal shift as a vergence
term (a fixation distance, not a percentage), radii in degrees rather than
NDC, and -- where a d3d11 proxy is better placed than an API layer -- masking
only the scene's geometry draws into the scene target, which the census
already classifies, so the HUD and the post passes are never coarsened. Pimax
Play's own DFR uses the same NvAPI path (LibMagicD3D1164.dll); two holders of
the shading-rate view cannot coexist, so this feature stands down when that
module is loaded. The vergence term applies to feature 6's disc as well.

**Built, fixed centre (2026-09-05, the foveation branch; `src/d3d11/foveation.cpp`).**
Sean's direction after the crisp-fovea discussion under feature 6: a DLSS
crop cannot meet "no blur where the eyes look" under upscaling, because a
crop never has history for where the eyes go next, so the gaze's value is
here, and the fixed-centre build comes first because it needs no gaze at
all. What was built:

- *NvAPI without vendoring.* The six entry points are resolved through
  `nvapi_QueryInterface` by the IDs in NVIDIA's `nvapi_interface.h` (MIT),
  and the three structures -- the V2 capabilities (64 bytes), the
  per-viewport rate table (68) and its container (16), the view description
  (24) -- are transcribed from NVIDIA's reference documentation with
  `static_assert`s on their sizes. Every structure carries its size in its
  version word, so a transcription error is refused by the driver rather
  than acted on; the two things the driver cannot check, the rate values
  and the view dimension, are what the desk probe measures
  (`edvrFoveationProbe` in tools/smoke: a 512x512 target drawn through an
  image whose tile rows name every rate, the shaded block size read back per
  row -- 1x1, 2x1, 1x2, 2x2, 4x2, 2x4, 4x4 must measure as themselves).
- *The image.* One R8_UINT texture per eye-texture size and eye, a texel
  per 16x16 tile, filled on the CPU: each eye's frustum from the tangents
  the openvr half publishes (widened by the guard's lie when live), the
  centre shifted toward the nose by the eye's offset over
  `foveation_distance` (the temporal pass's noted offset, or a 32 mm half
  IPD), and a tile's rate the finest its nearest point to the centre needs.
  Texel 0 = full rate, 1 = the 2x2 ring, 2 = beyond (4x4; 2x2 in quality).
- *Where it binds.* In the draw path, after the census's own eye-sized
  verdict on slot 0: bound (rates and view, both, every time -- the table is
  per-viewport state) when the target is eye-sized, cleared for anything
  else, at ClearState and at the frame boundary. The eye of a target: the
  submitted texture when it is one, else the depth probe's rule (of two
  targets alike in size and format, the first bound in the frame is the
  left). Instrumented: a 600-frame summary of targets per eye, how many were
  known by the submitted texture, and image switches per frame.
- *Settings.* `fix.foveation = off | quality | balanced | performance`
  (70/100, 50/84, 38/70 degrees across for the full-rate disc and the 2x2
  ring's outer edge); `advanced.foveation_inner`, `_outer` override the
  preset; `advanced.foveation_distance` (0.7 m); `advanced.foveation_passes
  = all | geometry` (the A/B for blocky bloom: geometry leaves draws of six
  vertices or fewer -- the full-screen passes -- at full rate, at the cost
  of the lighting pass's saving). All live.
- *Fail-safes.* No nvapi64.dll, no entry points, an initialise or create or
  set call not answering OK, a GPU reporting no support, or Pimax Play's
  `LibMagicD3D1164.dll` in the process: one line and off for the session.

**The desk (2026-09-05, RTX 5090, nvapi64.dll 32.0.16.1664).** Six rounds
of the probe, and what they settled:

- Every NvAPI call answers OK in every order tried (table then view, the
  Toolkit's and vrperfkit's order; view then table; one viewport or
  sixteen; the state set before or after the target; RegisterDevice first),
  and the capability query says the GPU shades at variable rate. The
  transcribed values were checked against nvapi.h read from NVIDIA's
  repository: the rate enum is 0..11 with 1x1 at 5, 2x2 at 8 and 4x4 at 11,
  the 2D view dimension is 4, the tile is 16, and the three structures are
  68, 16 and 24 bytes with a one-byte `bool` leading the viewport record.
- **The driver does not read the initial data of a shading-rate texture.**
  An image given its texels at creation reads as zeros: every tile takes
  texel 0's rate (mapped to 4x4, everything shaded 4x4; mapped to 1x1,
  "no effect"). The same bytes written by `UpdateSubresource` or copied in
  from a staging texture are read tile for tile. The module fills its
  images with `UpdateSubresource`, so the module was right from the start
  and the probe was wrong; the probe now does what the module does and
  keeps the initial-data variant as the documented quirk.
- With that, every rate measures as named -- 2x1 two wide and one tall,
  1x2, 2x2, 4x2, 2x4, 4x4 -- both by the position the pixel shader reports
  (the coarse pixel's, so it is a valid detector) and by a UAV counter of
  invocations per band (32768 at full rate, 8192 at 2x2, 2048 at 4x4).
- NvAPI's own VRS helper (`NvAPI_D3D_InitializeVRSHelper`, since R430, with
  `NvAPI_D3D_InitializeNvGazeHandler` beside it taking per-eye gaze) also
  shaded coarsely on this GPU by its own narrow pattern -- an alternative
  for the eye-tracked phase should the explicit image ever fail elsewhere.
  The probe round that read its pattern back did not come back (the smoke
  hung until a restart), so the helper is neither used nor probed.
- Two probe lessons: a full-screen triangle wound counter-clockwise is
  culled by the default rasteriser state and measures the clear colour as
  16x16 blocks; `near` is a Windows macro.

**Flight 1 (2026-09-05 14:54, `v0.14.0-27-ge11153b-dirty`, balanced).**
Armed at the first eye draw, bound, and running for the session: four
images made (both eye-sized sizes on this rig -- the 3252x3213 render and
the 4336x4284 output -- for each eye), about two distinct eye-sized
targets a frame, 2.6 image switches a frame, no tangent gaps. Sean's word
on the picture: "definitely more what I expected". On performance he saw
no large gain, and the log says why that cannot be read from this flight:
the session ran 81, 87, 89 and 90 frames a second by twenty-second
windows, at the 90 Hz cap for its second half, where a saving is headroom
and not frame rate; and the pass itself cost 2.8 ms an eye (NVIDIA's 1.9
of it), about 5 ms of an 11 ms frame that a shading rate cannot touch,
because the pass is compute. What the log cannot yet say is how many of
the frame's draws ran under the image. Next: an A/B with the live key
against SteamVR's GPU frame time in a heavy scene, a summary that counts
draws under the image per target size, and then the render scale up.

**Flight 2 (2026-09-05 18:15, `v0.14.0-29-g31b66f8-dirty`).** Sean
switched between the three presets in flight (balanced, performance,
quality, twice over) and saw no change in frame time; quality looked
best. The frame-rate windows say why nothing could show: 89, 88, 86, 90,
82, 84, 86, 85, 87, 86, 84, 85, 88, 84, 77 and 48 frames a second, and
the one thing they track is the draw count -- 695 eye draws a frame at
88, about 2,000 to 2,500 at 84 to 87, and 4,921 at 48. The previous
flight ran the same range at native 4336x4284 under DLAA; this one at
3252x3213 under DLSS, 1.8 times fewer pixels, at the same frame rates.
A frame whose time follows draws and ignores pixels is bound by the CPU's
submission, not the GPU's shading, and a shading rate cannot shorten it.
The draws-under-the-image summary landed in the loading screen (two draws
a frame, both under the image) and said nothing about the scene, so the
next build makes it a periodic census of every target the game draws
into, eye-sized or not, with the draws that ran under the image and the
ones that did not. The plan holds: the saving is spent on resolution,
which moves the frame's time to the GPU, where it can be measured.

**Flight 3 (2026-09-06 05:47, `v0.14.0-30-ge9ebb10-dirty`, quality).**
The census answers the coverage question: in the scene at 3252x3213 the
frame drew 357 times into eye-sized targets and every one of them ran
under the image -- 320 into the R10G10B10A2 main target, 14 into the
R8G8B8A8 one, 2 into an R32 -- while the 55 draws a frame that did not
went to the 3840x2160 mirror window, two 406x401 effect buffers and a
1920x1080 surface, none of them an eye. Mid-flight the render rose to
native 4336x4284 (DLAA) and its R10G10B10A2 target took 84 draws a frame
under the image. The coarse shading reaches the scene's passes; the
frame rate ran 87, 84 and 85 at 3252 and 77 at native. What the log still
cannot say is whether the GPU was the limit in any of those windows; the
next build samples the GPU's own busy figure through NvAPI and prints it
with the census.

**Built, the centre following the eyes (2026-09-06, the foveation
branch).** Phase 2 and Phase 3 of docs/eye-tracking.md in one build, on the
formula the sweep protocol named. The openvr half's gaze source
(`gaze_probe.cpp`, riding on the probe's arming and sentinel) computes
`normalize(p.x - t.x, p.y - t.y, t.z - p.z)` every frame from the two
point-form reads of entry 36 and the raw-universe head of entry 12, keeps it
only when its length is within a tenth of 1 and it points ahead, and
publishes it as head-frame tangents over the frame_flag channel (mapping
`_v23`), with "lost" published as a zero word so the reader falls back at
once. It arms only on Pimax's `aapvr` driver, where the repair was measured;
elsewhere one line and the fixed centre. The d3d11 half reads the channel
at each frame boundary: a fresh gaze moves the rings' centre, on top of the
nasal shift, once it has travelled past a 1.5-degree dead band from the
centre the images were last built on (a refill by UpdateSubresource, a few
kilobytes an image); a gaze published as lost, or silent for a second,
returns the centre to straight ahead with one line. `fix.foveation_centre =
eyes | ahead`, eyes the default; the tracker side reads it at launch, the
rings live. The summary names the centre's state and the refill count, and
now carries the GPU's own busy percentage, sampled once a second through
`NvAPI_GPU_GetDynamicPstatesInfoEx` for the whole GPU -- under 90 is a
frame the GPU is not the limit of.

**Flight 4 (2026-09-06 06:20, `v0.14.0-31-gef0f3f3-dirty`, native
4336x4284 under DLAA).** The centre followed the eyes from the first frame
-- the source armed on `aapvr` at head-frame tangents (+0.015, +0.070),
the rings refilled 673 times in 5,400 frames as the gaze crossed the
dead band -- and Sean's word was "seems to work well". The GPU figure
answered the question the earlier flights could not: 83 to 85 percent
busy through the menu, 82 with a peak of 98 as the scene arrived, and
**98 percent on average in the scene window** (260 eye draws a frame,
every one under the image, 69 frames a second, the performance preset).
So the GPU was the limit there, and the ring sizes still moved nothing.
That leaves the GPU's time somewhere the shading rate does not reach: the
pass's own compute at native (2.5 to 2.9 ms an eye, some 5.5 ms of the
14.5 ms frame), the game's compute-side lighting and post (its exposure
runs in a compute shader, which is how the exposure fix hooks it), the
G-buffer's bandwidth at 37 million pixels a frame -- or the image not
taking effect inside the game despite taking effect on the desk. The
next build carries the instrument that separates those:
`advanced.foveation_outer_rate = cull` leaves the tiles beyond the outer
ring undrawn, so the periphery goes black, which proves the image reaches
the game's pixels by eye alone, and the frame time under it is the
ceiling of what any rate could save on that scene.

## Feature 3 — the eye-tracked centre

**What changed since the toolkit era.** The toolkit needed a per-vendor
SDK for every eye tracker (Varjo, SRanipal, Omnicept, Droolon). Valve has
since built gaze into OpenVR itself (vendor-stated, the OpenVR SDK
release notes): from SteamVR 2.8.3 the OpenXR side advertises
`XR_EXT_eye_gaze_interaction`; from SDK 2.12.14 any third-party driver
can publish eye data (`Prop_SupportsXrEyeGazeInteraction_Bool`,
`CreateEyeTrackingComponent` / `UpdateEyeTrackingComponent`); and the
application side of current `IVRSystem` carries
`GetEyeTrackingDataRelativeToNow` / `GetEyeTrackingDataForNextFrame` and
`GetEyeTrackedFoveationCenter[ForProjection]` — the last two existing
*explicitly for foveation*. One vendor-neutral call, for any headset
whose driver publishes gaze: the Bigscreen Beyond 2e (whose foveation
work is being done with Valve directly), the Pimax Crystal's Tobii
tracker (a SteamVR driver shim demonstrating the path already exists),
and whatever adopts the driver API next. No SRanipal, no Tobii SDK, no
per-vendor adapters — which is also why per-vendor adapters are rejected
here rather than deferred: they are the thing this API exists to end.

**The rule this bends, bent in the open.** Elite links `IVRSystem_012`
(measured, the exe's interface literals), which predates gaze by a
decade; the methods live on modern interface versions EDVR would have to
request for itself. This project has a standing ban — the 6c rule,
`system_hook.cpp`: *IVRSystem is never called from inside the game* —
earned when a call into the 012 interface with a guessed ABI corrupted
the stack after appearing to work. This design asks for the first
deliberate exception, and the differences are the argument: the ban's
incident guessed the ABI of an *ancient* version from a modern header;
this requests a modern version by its own exact name, with that version's
own declarations, on EDVR's thread at the frame boundary, validated on
first call (a gaze ray has a shape: finite, unit-ish direction, origin
near the eyes — anything else means the pin is wrong, and the feature
goes inert loudly, never to guess). The exception is scoped to exactly
these calls; the ban stands everywhere else. If the maintainer prefers
the ban absolute, this feature waits — features 1 and 2 do not depend on
it, and fixed-centre foveation is most of the win.

**Where it cannot work, by construction.** OpenComposite-family stacks
implement the interface versions games use and will not serve a modern
`IVRSystem` (believed; confirmed or refuted in one log line at Phase 0) —
and players on those stacks already have OpenXR Toolkit and quad-views.
The launch centre already distinguishes `Runtime::Valve` from
`Runtime::OpenComposite` by export shape (`launch_centre.cpp`), so the
gaze path arms only where it can exist. The audience split is clean: this
feature serves real SteamVR, which is precisely where the new API lives.

**Mechanics.** Gaze is sampled once per frame at the `WaitGetPoses`
boundary (the hook exists; it is the same "before the game queries" point
every other per-frame decision uses). The ray maps to a per-eye image
point through the tangents in use — true ones, or lied ones under the
guard — and the mask regenerates when the centre moves a tile or more.
The mask is ~180×120 bytes at Quest-3-class sizes; regenerating it is
noise. Fallback is per-frame and silent: gaze invalid this frame (a
blink, a dropout) → fixed-centre rings; tracker gone → fixed rings until
it returns. A blink must not produce a log line.

**Privacy, stated the Explorer Cam way, because it is the same promise:**
a direction is consumed each frame, compared against the last mask
centre, and discarded. Nothing is logged, stored, or leaves the process;
the log says gaze is in use once, at arming, and nothing afterwards.

**Verification posture, as first written.** Neither field rig had an eye
tracker, so this was to ship the way the cull guard shipped for real
SteamVR: implemented, guarded, and asking for logs — the arming line, the
first-call validation verdict, and the mask-update cadence line the whole
ask. The eye-tracked centre is a small delta on feature 2 (the mask
generator gains a moving centre); the risk lives almost entirely in the
interface pin, which is why the pin validates before it acts.

**The route, settled (2026-09-04).** Three things changed since the
paragraphs above. The field rig now includes a Pimax Crystal Super, whose
tracker runs at 120 Hz, so the question can be put to a real driver. The
SDK that carries the call is known: 2.15.6, whose `IVRSystem_Version` is
`IVRSystem_026`, and the call as shipped returns per-eye NDC *points*
(`GetEyeTrackedFoveationCenter(HmdVector2_t* left, HmdVector2_t* right)`,
plus a variant that takes the projection matrix to use) rather than rays,
so the shape test is finite and within the image's neighbourhood. And the
6c exception is narrower than the paragraph above bargained for: OpenVR
serves every interface in a second shape, the C function table
(`FnTable:IVRSystem_026`), a plain struct of function pointers with no
hidden `this` and no hidden return slot — the two things the ban's
incident was about. The probe asks through that table, transcribed from
Valve's `openvr_capi.h` at the SDK whose version string it names, so the
index and the string come from one header. Before any gaze entry is
called, entry 0 must answer the same recommended render size the game
was told (the system hook holds the truth); a table that answers
otherwise is not the table, and nothing further is called. The first
calls run behind a crash sentinel, like the early handover, so a launch
that dies in them costs the next launch the probe and nothing else. It
only ever asks Valve's own runtime: OpenComposite raises a fatal dialog
for an interface it does not implement, and the launch centre's export
test says which runtime is underneath.

The probe (`advanced.gaze_probe`, off by default) logs one line at
arming, one at the first valid centre, then a summary every minute — how
many frames the runtime vouched for, declined (no tracker, or a blink),
or answered with something that is not a point; each eye's range, mean
and mean per-frame step; and whether the projection variant agrees with
the plain call. The step is the tell: a tracker that follows the eyes
moves a little every frame, a driver publishing a constant moves by
exactly nothing. No per-frame value is ever written — the privacy rule
above, kept by the instrument as well as the feature. One flight on the
Super, docked, answers whether Pimax Play's SteamVR driver publishes
gaze; a `declined` column at 100% with a plain `IVRSystem_026` served
means it does not, and the fixed-centre versions of features 2 and 6 are
what that headset gets until it does.

## Feature 4 — the in-headset menu

**Why it exists.** Every knob above is a taste-and-cost trade that can only
be judged with the headset on, mid-flight, over the scene that is actually
struggling. Today that judgement runs through alt-tabbing to a text editor
or the installer — workable at a desk, miserable over a planet. The
toolkit's menu, for all its looks, was the reason its features got tuned
at all. This is that idea, built the way this project builds things.

**What the toolkit's menu got wrong, which is the brief for this one:**
head-locked text that rode every head movement; pixel-fixed type that came
out tiny on wide-FOV headsets; three chorded function keys to navigate a
tree; and no sense of what a setting *cost* beyond watching the corner FPS
counter wobble.

**The shape of the fix, in five decisions:**

1. **World-anchored, not head-locked.** The panel spawns where you are
   looking, at a comfortable distance (~1.4 m, curved gently toward you —
   the `panel_curve` math, reused), and then stays put in the world while
   you read it: the theater's frozen-pose anchoring (`theaterXform`,
   reprojection-correct from inside Submit) is exactly this machinery.
   Re-summoning recentres it to wherever you look now. Fade in and out,
   ~150 ms; nothing else animates — motion on a panel is what looks cheap
   in a headset, not the absence of it.
2. **Type sized in degrees, not pixels.** Row text is specified as visual
   angle (minimum cap height ~1.1°, measured for comfort in Phase 0) and
   derived per headset from the tangents the system hook already captures
   — the same numbers that place the RemLok lines. A Pimax and a Quest 3
   get the same apparent size from different pixel counts. And the panel
   is composited *after* the guard crop and the upscale, onto the
   native-size outgoing frame, so its text is always native-crisp no
   matter how far `render_scale` is pushed — the menu never degrades with
   the setting it is adjusting.
3. **Look at a row; press one key.** Head-aim replaces up/down navigation
   entirely: the row under your gaze highlights (generous hitboxes,
   sticky hysteresis so head jitter never flickers it), and EDVR's own
   menu key activates — toggles toggle, enums cycle, sliders step, hold
   to repeat, with a chorded reverse for the other direction. When
   feature 3's gaze is live, the aim ray is your eyes instead of your
   head, and an optional dwell-to-select needs no key at all — the menu
   doubles as the eye tracker's live sanity check, since the highlight
   *is* where EDVR thinks you are looking. XInput pads navigate too
   (d-pad and face button — `xinput_watch` already polls pads
   edge-triggered for the FSS bindings); a HOTAS stays out of reach, as
   it already is, and head-aim is why that costs nothing.
4. **Keys that are checked, because they cannot be captured.** EDVR
   watches input and never blocks or injects it (`hotkey.h`'s polled,
   foreground-gated design — the Explorer Cam promise, unchanged), so any
   key the menu uses also reaches Elite. That is a fact to design around,
   not hide: the menu's keys default to chords Elite's stock bindings
   never use, and — the touch no other tool has — EDVR reads the
   player's *actual* bindings (`elite_binds`) and verifies the menu keys
   against them, warning in the log and on the panel itself when a menu
   key would double-act in the ship. Head-aim keeps the key count to
   two.
5. **Every setting wears its price.** Each row shows what its current
   value costs or saves, measured, not estimated: `render_scale` shows
   the percentage of pixels being rendered and the upscaler's own
   timestamp-query milliseconds beside it; foveation shows the rings'
   savings; the status page shows the frame time and every EDVR pass's
   share of it. The project's rule that every feature ships with its
   price measured, promoted from the log into the interface. This —
   more than any styling — is the "user friendly" the toolkit lacked:
   the player watches the trade they are making, live, over the scene
   they are making it for.

**What it shows.** Two pages at first, deliberately few rows each:

- **Performance** — `render_scale`, `render_sharpness`, `foveation`
  preset, the gaze centre on/off with a live tracking indicator. Each row
  carries a one-line plain-language hint (the ini's own explanations,
  shortened; `edvr.ini` remains the deep documentation) and a
  takes-effect-at-restart badge where that is true, exactly as the
  installer's screen says it.
- **Status** — the README's "checking it worked" section as a live panel:
  which runtime is underneath (the launch centre's verdict), the real eye
  size, guard state, scale state, whether VRS armed and if not why not,
  gaze availability, frame time with EDVR's passes itemised. Read-only,
  and the page a support thread will ask to see.

The row registry is data-driven off the same config keys, so later
live-tunable settings (panel distance, sun glare mode, the particle fix)
can join without new machinery — but the menu ships scoped to what this
design adds, plus status.

**Toasts, the menu's little sibling.** When a live setting changes — from
the menu, the installer, or a hand edit to the ini — a one-line
confirmation fades through the lower view for a couple of seconds:
`render scale 0.80 — rendering 64% of the pixels, upscaler 0.21 ms`.
Today that feedback exists only in the log; a player tuning by ini edit
deserves to see the change land without taking the headset off.
`menu_toasts = off` for players who want nothing uncommanded on screen,
ever.

**How it is built.** The panel is rasterised on the CPU — DirectWrite for
the glyphs, into a system-memory bitmap, uploaded once per *change*, not
per frame — so there is no font engine on the GPU and no per-frame text
cost; per frame the menu is one textured, alpha-blended quad per eye. The
split follows the theater exactly: the openvr half owns the decision to
show, the anchor pose and the input; the d3d11 half owns rasterisation
and the draw, behind one export (`edvrMenuPanel`, working name), standing
down loudly on a mismatched pair. While the menu or a toast is up, frames
route through an EDVR-owned copy (the shadow path that already exists) so
the game's own textures are never drawn on; when nothing is shown, the
whole feature is one hotkey poll per frame.

**Persistence, with one source of truth.** The menu writes `edvr.ini` —
surgical value edits that preserve every comment, by the same config
grammar `config.cpp` reads and the installer's merge already parses
(`iniedit`'s contract: a merge that disagreed with the reader would write
a file whose settings are not the ones it reports; the same holds for a
menu). Changes apply in memory immediately and the file carries them to
the next session; the installer's settings screen and the menu can never
disagree, because neither owns any state the other lacks. The desktop
screen remains the place for everything; the menu is the flight-relevant
subset.

**Safeguards, because they are the reason to trust it:** it never appears
uncommanded (toasts are the one exception, and they have an off switch);
it auto-dismisses after idle seconds; it draws only into EDVR's own
copies, never the game's textures; its keys are watched, never taken —
the game receives every press, which is why the collision check exists;
and a rasterisation or draw failure means no menu and one log line, with
the game unaffected. Closed, it costs one key poll; open, it costs the
copy path and says so on its own status page.

**Settings sketch** (`[hotkey]` and a new `[menu]` section):

```
[hotkey]
menu            = CTRL+ALT+E   ; summon/dismiss; verified against your binds
menu_adjust     = SPACE        ; activate/step the aimed row (chord reverses)

[menu]
distance        = 1.4          ; metres; live
idle_dismiss    = 20           ; seconds; 0 = stay until dismissed
menu_toasts     = on           ; the transient change confirmations
```

Defaults chosen at implementation after a sweep of Elite's stock binding
sets; the collision check, not the choice, is the real safety.

## Feature 5 — the performance monitor

**The ask, verbatim from the field:** a lightweight readout at a
configurable point in space *relative to the centred view* — because the
toolkit head-locks its overlay front-and-slightly-above, and a thing that
rides your head is a thing you cannot stop seeing.

**An instrument, not a HUD.** The monitor is anchored in the seated
tracking space — the space whose zero *is* Elite's centred view, and the
space the launch centre already works in (it asks the runtime to move
this very origin rather than subtracting from poses). Anchored there, the
monitor behaves like a gauge screwed to the cockpit: it sits where you
put it, you glance at it when you want it, and it leaves your view when
you look away. When you recentre with Elite's own reset binding, the
seated zero moves and the monitor follows the new centre without any
help — which is the whole meaning of "relative to the centred view".
It also stays put through Explorer Cam and on foot, because it lives in
your play space, not the game's.

**Placement is three live numbers** — yaw and pitch off centred forward,
and distance — plus the stylish way to set them: a "move it here" row in
the menu. Activate it, the monitor follows your aim; press the key and it
drops where you are looking, with the coordinates written back to the
ini. The shipped default sits low, roughly where a fuel gauge lives
(~14° below centre, 2 m out, small), clear of Elite's own cockpit UI —
the exact spot is a Phase 0 comfort measurement, not a guess. Its size,
like the menu's type, is specified in degrees and derived per headset.

**What it shows, and no more.** Four fields, smoothed, redrawn at 2–4 Hz
so digits never flicker: frames per second and frame time (the submit
pace machinery already measures every frame boundary and counts long
frames — measured, it exists today); GPU frame time, by a timestamp
query pair bracketing Present on the immediate context (believed sound
on this single-context renderer; Phase 0 checks it against known
workloads); EDVR's own passes, itemised in microseconds — the monitor
measures itself and says its own price; and the long-frame count over
the last half minute, which is the number that tells you whether the
judder you felt was real. A slow sparkline of frame time is the one
moving element, and it can be turned off.

**Distraction is managed by design, not asked to be tolerated:**

- `show = drops` mode: the monitor stays invisible until the pace
  machinery sees a long-frame burst, fades in for a few seconds around
  it, and leaves. Perf numbers exactly when something is worth
  explaining, an empty cockpit the rest of the time.
- **Glance-to-wake**: at rest the panel idles at low opacity; when your
  head ray (your gaze, once feature 3 is live) lands on it, it brightens
  to read. The rays are already computed for the menu's aim; the monitor
  reuses them.
- Nothing animates but the sparkline, and the sparkline crawls.

**Why not point players at SteamVR's own frame graph:** it lives in the
dashboard — not glanceable mid-fight, and on OpenComposite rigs there is
no dashboard at all (measured; its absence is how EDVR detects
OpenComposite). An in-world instrument serves both field rigs and real
SteamVR alike.

**How it is built.** The menu's machinery, third client: the same
DirectWrite rasterise-on-change, the same per-eye quad draw, the same
seated-space transform the "move it here" placement uses — one renderer
serving the menu, the toasts, and this. While `render_scale` is live the
monitor draws into the upscaler's output texture, which exists anyway —
zero additional copies; with scale off, showing the monitor routes
frames through the EDVR-owned copy path and the status page says so.
Never onto the game's textures, like everything else here.

**Settings sketch** (`[monitor]`):

```
show     = off    ; off | always | drops (appear around dropped frames)
yaw      = 0      ; degrees right of centred forward; live
pitch    = -14    ; degrees above centred forward (negative = below); live
distance = 2.0    ; metres; live
glance   = on     ; dim until looked at
```

## Feature 6 — DLSS where you look

**Why it exists now.** Feature 2 shades the periphery coarser and leaves
the pixel count alone. The temporal work added a second cost that scales
with pixels: NVIDIA's DLAA and DLSS run over the whole output, and their
price follows output size — measured at 2.7 ms per eye on the Pimax
Crystal Super at HMD Quality 1.0 (roughly 31 megapixels per eye, against
8 for a 4K monitor), 1.0 ms per eye on a Quest 3, 5.4 ms of an 11.1 ms
frame at 90 Hz on the Pimax. Nothing in the rings touches that. A pass
that runs only where the player is looking does.

**What it is.** NVIDIA's runtime evaluates a sub-rectangle of larger
buffers: the DLSS feature is created with output sub-rectangles enabled
(`InEnableOutputSubrects`), and each evaluation names a base and size for
the colour, depth, motion-vector and output regions
(`InColorSubrectBase`, `InDepthSubrectBase`, `InMVSubrectBase`,
`InOutputSubrectBase`, `InRenderSubrectDimensions` — vendor-stated, the
310.7 headers). The pass keeps every input it already builds for the full
frame — the jittered colour, the depth copy, the motion vectors — and
hands NVIDIA a crop around the gaze point, at the crop's fixed size; the
periphery outside the crop goes through EDVR's own temporal history, which
exists and is tuned, or through the resolve alone. The two are composed at
the door with a soft edge. Cost follows the crop's area: a crop a third of
the frame's width is a ninth of its pixels, and 2.7 ms per eye becomes a
fraction of a millisecond.

**The centre.** Feature 3's gaze point, mapped through the eye's tangents
exactly as the rings are, with the same fallback: a fixed centre — the
straight-ahead point, which is not the texture centre in an asymmetric
frustum — whenever gaze is invalid or absent. The fixed-centre version is
a feature in its own right, and the one every headset without a tracker
gets: the middle of the view is where the cockpit's text and the target
sit, and it is measurable on both field rigs before any tracker is
involved.

**Built, fixed centre (2026-09-05, the foveation branch).**
`advanced.temporal_aa_fovea` (degrees across the fovea, 0 = whole frame),
`advanced.temporal_aa_fovea_shape` (round, the default, or square),
`advanced.temporal_aa_fovea_edge` (the blend band, degrees, inside the
fovea), `advanced.temporal_aa_periphery` (steady, the default, or sharp),
`advanced.temporal_aa_periphery_scale` (the steady periphery's size as a
fraction of the output, 0.5) and `advanced.temporal_aa_periphery_calm` (the
sharp periphery's easing), all live. It works under both `temporal_aa =
dlaa` (the crop 1:1) and `temporal_aa = dlss` (the game renders small,
NVIDIA upscales just the crop to native). `src/d3d11/dlaa.cpp` runs up to
three NGX features per eye, each with its own history: the full frame, the
fovea crop (`dlssEvaluateFovea`, created at the input crop -> output crop
with output sub-rectangles) and the periphery (`dlaaEvaluatePeriphery`,
DLAA over a reduced copy). In `temporal_pass.cpp` the trained inputs are
built once (the colour copied out typed, the motion vectors and depth copy
at render size); a reduction shader boxes them down for the periphery
(colour mean, nearest depth, that sample's motion scaled into the reduced
pixels, the jitter scaled the same way); NVIDIA evaluates the periphery and
the crop; and a composite shader blends the crop over the periphery --
upscaled with a Catmull-Rom bicubic when it is smaller -- through a
smoothstepped disc (an ellipse where the crop is clamped). The own pass
does not run at all in that mode. With `periphery = sharp` the own pass
fills the periphery at full size, eased mildly toward the frame's edge, and
the composite writes its blend back into the own history so content
leaving the fovea carries NVIDIA's pixels out with it and decays over
frames instead of stepping. The centre is the straight-ahead point from the
effective tangents; the output crop is the input crop scaled exactly, so the
fovea and periphery register. Skipped entirely at width 0, so an unchanged
config is byte-for-byte the plain trained path. The smoke harness drives the
whole pipeline on the desk (both peripheries, three frames each, the
composite counted) through a dev hook, beside the NGX slot checks.

**Flown, and what the seam taught (2026-09-05).** The first builds paired
NVIDIA's crop with the own history in the periphery, and three flights on
the Pimax Crystal Super at HMD Quality 1.0 (4336x4284 per eye; a 40-degree
fovea is a 1232-pixel square, 8.2% of the pixels, 0.32 ms per eye against
2.7 full-frame) met the seam in three forms: periphery shimmer under head
motion; then, after a calming that made the periphery's history heavier, an
"underwater" smear the player saw wherever the eyes went; and finally, with
the calming inverted, a square outline that showed under small
back-and-forth head movements as "a lack of continuity". The mechanism is
temporal, not spatial: the own history resamples itself every frame, so it
blurs while the head moves and re-sharpens when it stops, while NVIDIA's
crop does neither, and the boundary between a breathing region and a still
one pulses -- no blend band hides that. Two more findings shaped the fix.
With a fixed centre the player looks straight at the periphery whenever the
eyes move, so it cannot be treated as peripheral vision, and any deliberate
softening there is seen. And the own pass is not cheap at that size -- the
totals put it near 1.3 ms per eye -- so crop-plus-own saved 0.8 ms of the
2.7, not the 2.4 the crop alone suggested. The steady periphery answers all
three: both sides of the seam are NVIDIA's and neither breathes; the
periphery is softer (half the pixels each way, upscaled) but steady, the
trade the player had already proposed for the half-render variant; and the
whole arrangement -- crop 0.32 ms, periphery about a quarter of full-frame,
reduction and composite a fraction -- costs less than the crop-plus-own it
replaces. The disc came from the player's own question: the eye picks out a
straight edge and a corner at far lower contrast than a smooth radial
gradient.

**Flight 2 (2026-09-05 09:15, scale raised live to 0.7).** The disc reads
as round. What remains: a "noticeable shift" at the seam, and a lag after
the head stops before the fovea blends into the scene. One cause is in the
log: at 0.7 the reduction dropped a 2x2 box at floor(p * 1.43), so each
reduced pixel's sample sat up to 0.6 render pixels off its true position in
a seven-pixel pattern -- exact only at a half scale, where the flight
spent its first three seconds. That is a real displacement of the periphery
against the fovea, and as content slides across the pattern under head
motion NVIDIA sees per-pixel position noise, converging again only once
the head stops. The reduction is now an area-weighted box, position-exact
at any ratio. What cannot be removed that way is the two networks' different
response to the same head motion at two pixel scales (half the pixel motion
in the periphery, a box-filtered input): inherent to two instances, and
smallest where both read the same render pixels -- the half-render variant
(`temporal_aa = dlss` at HMD Quality 0.5), where the periphery is the render
itself and only the network mode differs. The first flight decides the default
scale and whether the sharp periphery keeps a purpose.

**Flight 3 and the desk (2026-09-05, later).** At 70 degrees, on dlaa and on
dlss, two things remained. The disc's edges were "still pretty clear", and
"it almost feels like the fovea disc is set in space away from me, rather
than like glasses over my eyes"; and the centre of vision blurred while the
head moved, becoming crisp a moment after it stopped. The first is
geometry: each eye's disc was centred on that eye's own straight-ahead
direction, and two parallel directions meet at infinity, so in stereo the
disc's edge fused as an object far behind the cockpit and the splash panel
-- and, screen-locked, it slid over them as the head turned, a distant
object following the head, which nothing in the world does and which the
visual system picks out even where acuity is low. Glasses read as "on my
face" because their frames sit at a near depth. `temporal_aa_fovea_distance`
(metres, live, 0 = infinity) shifts each eye's disc toward the nose by the
runtime's eye-to-head offset over that distance, so the edge fuses at the
depth of what is being looked at; OpenXR Toolkit ships the same shift as a
fixed 4% of the half-width. The second was settled by a desk probe
(`dlaaMotionProbe`, run by the smoke harness): the crop probe's scene
panning 6 px/frame for eighteen frames then standing still, evaluated on
identical inputs by the full-frame feature, the fovea crop and a half-size
frame. The crop matched the full frame within 6-8% under the pan, on the
first still frames and at rest; the full frame's own error under the pan
was 1.38x its rest error, recovering within five to seven frames. So the
softening in the centre is the model's own, present in full-frame DLAA all
along and hidden there by being uniform; a periphery that holds a fixed
reference makes it visible. The lever for that is the model itself --
NVIDIA's render presets, which the same probe now sweeps (DLAA defaults to
K, Performance to M, so the dlss configuration already runs two networks
across the seam). The sweep's numbers, error 0..255 in the crop's interior,
pan / first still frames / at rest: full-frame DLAA under K 2.98 / 2.58 /
2.15 (the default is K: identical), J 3.22 / 2.70 / 2.30, the deprecated
CNN presets E 4.40 / 4.09 / 3.36 and F 7.56 / 5.57 / 5.01. No preset
softens less under motion than K (1.38x its rest error; J 1.40x), and the
frame delta told to the model (0, 11.1 or 33.3 ms) changed nothing. DLSS
Performance 2x from a point-sampled half-size frame, against the full-size
truth: the driver's default M 13.35 / 11.84 / 14.27 -- worse at rest than
under the pan, barely below its first frame of 23.59 -- against K 9.41 /
7.65 / 5.34, J 9.32 / 7.53 / 5.23 and L 8.30 / 7.22 / 6.69. So
`temporal_aa_model` (quality = K for every mode, the default; responsive =
J; auto = the driver's choice) forces K, which leaves DLAA exactly as it
was and gives the dlss fovea the periphery's network.

**The bulge under a rapid nod (flight 4, 2026-09-05 11:17: dlss at about
Quality 0.65, fovea 70, fixation 0.8).** "When I move my head up and down
rapidly, it kind of makes it look like I'm causing the scene to bow in and
out at the eye point." The probe grew two measurements and a fast pan.
Positional lag -- the truth shift that fits each series' output best -- is
0.00 frames for every series at both 6 and 24 px/frame: with exact vectors
the model does not trail the motion, so the geometry across the seam is
exact and the bulge is not a displacement. The dlss fovea's exact path
(Performance 2x on a crop with output sub-rectangles) matches Performance
2x on the whole frame (1.02x under the pan). What the fast pan did find: at
24 px/frame -- a nod's speed at this frame size -- the fovea crop is 27%
worse than the full frame during the pan and 23% worse at rest ten frames
after it, where at 6 px/frame the two matched. That is the crop's history
boundary: the pan replaces the crop's whole content within the eighteen
frames, and content that has just entered the crop has no history, while
the periphery's DLAA covers the whole frame and keeps its history for
everything that moves within it. Under a rapid nod the centre is therefore
largely fresh -- less converged, its texture visibly different from the
periphery's -- and it re-converges once the head stops: a texture change at
the eye point, not a warp. Two remedies belong to the design, not to a
preset: a history margin (NVIDIA's crop larger than the visible disc, so
content has accumulated before it is seen; costly at a nod's speed, since
the margin must cover several frames of motion), and a content-following
crop (the crop's base slides with the content during fast motion, the shift
folded into the vectors the way the moving-crop probe validated, and
re-centres at rest -- which is what an eye-tracked crop does by itself,
since the eyes hold a target while the head turns). What a preset can do:
under the fast pan the upscaling mode's K softens 1.78x against L's 1.24x
(pan 9.40 against 8.71, rest 5.29 against 7.02), so `temporal_aa_model =
steady` -- K for DLAA and the Quality modes, L for the upscaling ones -- is
the shipped default: a little less at rest in the dlss fovea, much less
swing when the head moves.

**The design that gives a crisp DLSS fovea (2026-09-05, the answer to "no
blurring where my eyes look").** The requirement, in Sean's words: DLSS
upscaling the foveated area to native resolution, no blurring at all where
the eyes are directly looking, the edges free to blur. Everything measured
on 2026-09-05 bears on it:

- DLSS upscaling needs about 25 to 30 frames of history to be crisp; DLAA 5
  to 7. A crop loses history for anything that crosses its edge, and the
  soft zone is head speed times that convergence time, on the leading side
  of the motion, where the eyes go during a turn. The full frame keeps its
  history for everything, because content moving within the frame stays
  within it.
- The geometry is exact (positional lag 0.00 frames for every feature at two
  pan speeds), and the crop with output sub-rectangles matches the full
  frame in every mode. Nothing here is a defect to fix; it is what a crop is.
- The price, on this GPU at the Crystal Super's sizes (the cost probe, ms
  per evaluation, desk; the field runs about 1.6x these under the game's
  load): the full frame at native output costs the same whatever the model
  -- DLAA K 1.72; Balanced 2818->4336 K 1.65, L 1.54, M 1.54, J 1.55;
  Performance 2168->4336 K 1.55, but L 2.77 and M 2.21 -- so a cheaper
  model on the full frame buys nothing, and under DLAA preset L costs 8.53,
  five times K. The periphery's DLAA: 0.79 at the 0.65 render, 0.56 reduced
  to a half. The flown 70-degree fovea crop (Balanced 1542->2372): K 0.62,
  L 0.68. A 40-degree eye-tracked crop (Balanced 920->1416, L): 0.35.

So a fixed-centre crop cannot meet the requirement under upscaling: to cover
where the eyes might look it must be 60 degrees and more, and to keep its
history under head motion it needs a margin of several frames of motion each
side, at which point it is the frame. The two designs that do meet it:

1. *Full-frame DLSS.* History everywhere, crisp everywhere, no seam. 1.65 ms
   per eye on the desk at Quality 0.65 (about 2.7 in the field), and the
   model does not change the price, so `temporal_aa_model = quality` (K) is
   right there. This is what Sean found crisp in the A/B, and it is the
   fallback whenever the tracker is absent or lost.

2. *The eye-tracked fovea.* A small crop -- 30 to 40 degrees, the fovea and
   parafovea plus the tracker's error and a frame of latency -- centred on
   each eye's own gaze point (vergence comes with it; no fixation-distance
   term). Its history survives because the crop moves WITH the content the
   eyes hold: under fixation, smooth pursuit and the vestibulo-ocular reflex
   the gaze point travels across the screen with the content, and the crop's
   shift is folded into the motion vectors handed to NVIDIA -- the
   convention the moving-crop probe validated (a pan converges like a still
   crop; a jump costs one frame of fresh quality and twelve frames of
   convergence under DLAA). A dead band keeps the crop still while the gaze
   stays within the inner part of the disc, so small eye movements never
   move it; when the gaze nears the edge the crop re-centres, the content
   under the gaze keeping its history. The periphery is DLAA on a reduced
   copy (a half; less once it is never looked at), bicubic to native, and
   the fovea crop runs preset L (fastest from fresh content, least softening
   under motion; the model key's `steady`). What remains is the saccade: the
   eyes jump to a target that was in the periphery, the crop follows, and
   the content under the new gaze has no fovea history. The composite fades
   the fovea in by its history age over about twenty frames, so the gaze
   sees the periphery's converged image (65% of native at Quality 0.65)
   resolving to native over 0.2 s, the first 0.1 s of which saccadic
   suppression hides. That resolve is the physics of temporal upscaling and
   the only residual; everything else is crisp every frame. Price: about
   0.35 + 0.56 + a composite, roughly a third of the full frame -- and the
   same gaze then drives feature 2's shading rate for the game's own cost,
   which is the larger prize.

The fixed-centre fovea stays as a Quality 1.0 DLAA feature, where the fresh
content converges in a few frames and the arithmetic favours it (about 3.4 ms
a frame saved), with its limits recorded above.

**Where the crop design stands (2026-09-05, after the eye-tracking
discussion).** Sean's two objections settled it: eyes jump where heads
stream, so an eye-tracked crop is crisp for a jump that lands inside it and
resolves for about 0.3 s after one that does not -- and a real look is eyes
first, head after, so the resolve sits at the start of every large look.
Under upscaling no crop meets "no blur where the eyes look", structurally:
crispness needs history, history needs coverage, and a crop never covers
where the eyes go next; NVIDIA's history cannot be seeded from the
periphery; at a given render resolution the full frame is the ceiling for
the gazed region and a crop is only cheaper (about 2 ms a frame of the pass
in the field). So the gaze's value is feature 2, whose shading rate has no
history to rebuild, and the eye-tracked crop comes off the plan. The
fixed-centre fovea stays as the DLAA feature documented above.

*Reference: CheekyFoveatedDLSS (github.com/ClarkCheekyKent, read
2026-09-05), the same design elsewhere.* A ReShade add-on that intercepts a
game's own DLSS Super Resolution call (D3D11 and D3D12, OpenXR stereo for
VR) and replaces the full-frame upscale with DLSS on a centre crop plus DLAA
(preset E, the fastest) on the periphery downscaled to 0.75 of the render
and brought back; the crop's edge feathered over 4% ("Transition width"),
rectangular to elliptical by a "Roundness" control; the fovea 0.55 x 0.45
of the frame by default with a "Stereo X offset" moving the two eyes'
regions in equal and opposite directions (the nasal shift); eye tracking
through XR_EXT_eye_gaze_interaction with 20 ms smoothing, falling back to
the fixed centre; and, in `crop_motion.hpp`, the crop's shift folded into
the motion vectors exactly as the moving-crop probe above validated
("previousLocal = currentLocal + sceneMotion + currentOrigin -
previousOrigin"), with DLSS history RESET above the larger of 64 px or 12.5%
of the crop -- the post-saccade resolve, by design. It claims 2 to 3 ms a
frame saved on the DLSS pass, which is what this branch measured. Nothing in
it escapes the physics above, and its README makes no claim about the
fovea's crispness under motion; it lists "OpenVR-only games" as
unsupported, so it cannot run on Elite at all. Two things worth taking from
it if the crop is ever revisited: a periphery at 0.75 rather than 0.5
narrows the gap a resolve crosses, and the fastest DLAA preset is enough
for a periphery.

**Phase 0 for the eye-tracked fovea.** The gaze route on the Pimax (Route A,
the client-side repair of the mis-framed data SteamVR hands out, or Route B,
Pimax's own runtime; the plan and the probe protocol are on the eye-tracking
branch); the post-saccade resolve time of an upscaling crop after a 200-px
jump (the crop probe with an upscaling feature; this sets the fade's
length); the dead band against the tracker's noise, in flight.

**What must be measured first (Phase 0 items 15 and 16).**

- *History under a moving crop.* NVIDIA's history is kept in output space
  and reprojected by the motion vectors it is given. When the crop's base
  moves with the gaze, content that was inside the crop last frame sits at
  a different output pixel this frame by exactly the base's shift, which
  is a uniform screen-space motion the pass can add to every motion
  vector — a pan, in NVIDIA's terms, and pans are what motion vectors
  exist for. Content entering the crop from outside has no history and
  is reconstructed from the current frame alone, at the crop's edge,
  which is the periphery of the fovea. Whether the runtime behaves this
  way, or resets, or smears, is decided at the desk in the smoke harness:
  a full-frame synthetic scene, a crop that moves by a known step per
  frame with the step added to the vectors, and the crop's output compared
  against the same scene evaluated at a fixed crop. A saccade is the
  large-step case of the same test. **Measured 2026-09-04, and the design
  holds** (Phase 0 item 16 has the numbers): with the shift in the
  vectors a crop moving four pixels a frame converges to within a tenth
  of a still crop's error; without it, or with the sign wrong, it is
  ten times worse; a saccade of 200 pixels costs one frame of
  fresh-history quality at the new place and is converged twelve frames
  later.
- *The seam.* A crop's edge is a boundary between NVIDIA's reconstruction
  and EDVR's, and under a saccade it moves at the tracker's latency. A
  blend band in degrees of visual angle, wide enough that the eye is never
  on it, is the design; how wide is a headset-on judgement, made with the
  fixed centre first (look away from the middle and find the band), then
  with gaze.

**What it does not do.** It leaves the game's cost alone: the render
target is still full size, and the game still shades every pixel of it —
that is feature 2's job, and the two compose (a coarsely shaded periphery
under NVIDIA's reduced DLAA, a full-rate fovea under its crop). It is
NVIDIA-only by construction, like the rest of the DLSS path. And the
periphery's resolution is the steady periphery's scale — softer than the
fovea, visible on a fixed centre whenever the player's eyes leave the
middle, which is the reason an eye-tracked centre (feature 3) is the real
prize: with the fovea following the gaze the periphery is only ever seen
peripherally, and the scale can drop further.

**Settings sketch** (`[fix]`, final names at implementation):

```
temporal_aa_fovea = 0     ; degrees of visual angle across the crop; 0 = whole frame
                          ; (today's behaviour). Live.
temporal_aa_fovea_edge = 6 ; the blend band, in degrees. Live.
```

The crop's size in pixels follows from the degrees and the eye's tangents,
so one number means the same thing on every headset. The centre comes from
feature 3 when it can and the straight-ahead point when it cannot; there is
no key for that, because there is no wrong answer to prefer.

## Phase 0 — what must be measured, not assumed

One instrumented session (plus desk checks against SDK headers) before or
during implementation, in the head-steer convention:

1. **Eye-texture formats in the field.** The exact DXGI formats (sRGB?
   typeless?) Elite submits on both rigs, for the EASU/RCAS views. The
   pass refuses anything unmeasured.
2. **EASU/RCAS edge taps under the fused crop.** RCAS reads a 3×3
   neighbourhood, EASU a 12-tap ring; at the crop's edge those taps must
   clamp inside the region or the margin bleeds in. Decides fused vs
   sequential for v1.
3. **The upscale pass's price.** Timestamp queries, both rigs, quoted in
   the log the way the guard quotes its margins.
4. **NVAPI caps on the target GPUs.** Tile size (16 assumed), available
   rates, and behaviour on hybrid-GPU laptops (believed fine; NVAPI
   resolves the render GPU, but believed is not measured).
5. **Which passes tolerate coarse rates.** The artifact walk: presets
   against stars, bloom, smoke, planet rims. The census names any draw
   that needs excluding.
6. **The gaze interface pin.** The exact modern `IVRSystem_0xx` version
   string to request (from the SDK header at implementation), that
   SteamVR serves it alongside the game's 012 in one process (believed,
   from how vrclient versions interfaces; one log line settles it), and
   the first-call shape validation against a real tracker.
7. **Interplay flights.** Guard + scale composed; a withhold under scale;
   the theater under scale; a SteamVR resolution change *while* scale is
   live (the size channel should follow; watch it do so).
8. **DirectWrite beside the game.** CPU rasterisation into a
   system-memory bitmap in the game's process is believed inert; measure
   that it is (no device interop, no per-frame cost, no loader surprises
   at first use mid-session).
9. **Legibility, in degrees.** The minimum comfortable cap height and row
   spacing at 1.4 m on both rigs — the number the panel's whole layout is
   derived from.
10. **Head-aim comfort.** Hitbox size and highlight hysteresis that a
    resting head does not flicker and a deliberate glance lands first
    time; measured with the panel up, not reasoned about.
11. **The collision sweep.** Menu-key defaults checked against Elite's
    stock keyboard and pad binding sets, and the live check exercised
    against a real player's binds file.
12. **The seated space, confirmed.** That the poses at Submit are the
    seated universe and Elite's own reset binding moves its zero
    (believed from the launch-centre work, which operates on exactly
    this origin); one flight — recentre, watch the monitor follow —
    settles it.
13. **GPU timing honesty.** The Present-bracketing timestamp pair
    against known workloads (a render-scale step is itself a calibrated
    change), so the monitor's GPU number is one a bug report can be
    trusted to contain.
14. **The monitor's spot.** A default placement that clears Elite's
    cockpit UI across ship types on both rigs, and the glance
    hysteresis that wakes it on a look without waking it on a pass.
15. **The gaze answer, on the Super.** `advanced.gaze_probe = on`, one
    docked session on the Pimax Crystal Super under Valve's own SteamVR:
    the arming line says whether `FnTable:IVRSystem_026` is served and
    validated; the summaries say whether the driver vouches for a centre
    at all, how often, and whether it moves. This decides whether that
    headset gets the eye-tracked versions of features 2 and 6 or the
    fixed-centre ones. **Flown 2026-09-05, docked, three minutes, SteamVR
    2.17.8: the driver publishes gaze.** The table was served, entry 0
    answered the 4340×4284 the game was told, and all 12,600 frames came
    back valid with the centre moving by 0.0015–0.0021 NDC a frame. The
    values were the second finding: the two eyes differed in x by a
    constant 0.388, which is exactly twice the frustum's horizontal offset
    for these tangents (l −1.529, r +1.032), so both eyes report one shared
    direction projected through their own frustums, and the projection
    variant agreed with the plain call to four decimals. That direction
    sat at 37° right and 54° up for the whole session — just outside the
    visible field, where eyes cannot rest. The tracker had never been
    calibrated in Pimax Play (calibrated after the flight); the other
    candidate is the driver's vector arriving in a frame the runtime does
    not expect. The next flight separates them with the instruments added
    the same day: summaries every 450 frames read as directions, with the
    head's position and facing beside them. Look straight ahead; close
    the eyes (validity should drop for a real tracker); look far left,
    then far right, eyes only; then turn the head 30° while holding the
    gaze on one spot — a centre that swings with the head is in the wrong
    frame, one that holds is head-relative as designed. **Flight 2,
    2026-09-05, calibrated, that sequence in five-second windows:** the
    constant stayed (37° right, 54° up), so calibration was not the cause;
    the eyes-closed window froze the value (per-frame step 0.0001, the
    stillest of the flight) while staying 100% valid, so it is eye data
    and the runtime never reports a blink; a full eye sweep left and
    right moved the yaw by only about +6 and −5 degrees; and a 50° head
    turn with the gaze held dragged it +9° the *same* way. A head-relative
    gaze direction would have swung tens of degrees on the sweep and the
    opposite way on the turn. All three fit one explanation: the runtime
    is projecting a *point in the room's standing space* as if it were
    head-relative — a short gaze vector added to a room-space origin, so
    eye and head motion move it by centimetres, and the constant is the
    origin itself. Solving the constant for that origin gives about
    (0.67, 1.20, −0.72) m, and 1.20 m is a seated eye height. The probe's
    room test (built the same night, unflown) reads the runtime's
    seated-to-standing transform (entry 13 of the table) each window and
    prints where the head sits in standing space and what that point
    would project to; a match closes the case, and the finding then
    belongs to Valve or Pimax, since no consumer of the NDC can undo a
    projection whose depth it does not know. **Flights 3 and 4,
    2026-09-05: neither room frame is it, and the value is not usable
    gaze.** The runtime's standing space is itself uncalibrated on this rig
    (the head reads 5 m below and behind the standing origin — no SteamVR
    room setup for a headset that does its own tracking), and the raw
    universe came back identical to it (raw-to-standing was the identity
    transform), so both candidate frames collapsed to one and neither
    projects in front. The decisive test was translation: standing and
    stepping half a metre to the right moved the reported centre by about
    7° of yaw and it *held* at the new value, when a genuine gaze direction
    is invariant to where the head is. Combined with the earlier flights —
    frozen but still "valid" with the eyes shut, ~5° of swing for a full
    eye sweep, ~9° dragged the *same* way as a 50° head turn — every input
    produces a small, damped, sometimes physically backwards response
    around a large fixed offset (37° right, 55° up, just past the frame's
    top-right corner). That is not a gaze vector in any frame a client can
    read; it is a mis-framed, heavily damped value the driver flags valid.

    **Why, and what it means for the plan.** Pimax runs its own dynamic
    foveated rendering inside Pimax Play at the runtime level, so its
    SteamVR driver has never needed to expose true head-relative gaze
    through OpenVR's client API — a third-party shim existed precisely to
    add eye-tracking to Pimax's SteamVR driver for apps that want the
    standard gaze path. `GetEyeTrackedFoveationCenter` answering `true`
    with a damped, mis-framed constant is consistent with that: the API is
    served but not fed real gaze in the frame the header promises. So
    **feature 3's eye-tracked centre is blocked on this driver**, not on
    EDVR — there is no transform a client can apply to recover a gaze
    direction from what comes back. The probe stays in the tree
    (`advanced.gaze_probe`) to re-answer the question the instant a driver,
    a Pimax Play update, or a shim changes it, and to check other headsets.
    Everything downstream proceeds on the **fixed centre**, which needs no
    tracker and helps every headset: features 2 and 6 build and measure on
    the straight-ahead point on both field rigs. One wrinkle to hold for
    feature 2: on the Pimax, its own DFR already coarsens the periphery, so
    EDVR's VRS there would overlap it; feature 6 (the DLSS crop, which cuts
    NVIDIA's pass cost, not the game's shading) is additive regardless and
    is the one to build first.
16. **NVIDIA's history under a moving crop.** The smoke harness test
    described under feature 6: a synthetic full-frame scene, a crop that
    moves a known step per frame with the step folded into the motion
    vectors, and the output compared against a fixed crop's. Decides
    whether feature 6's moving fovea is a uniform-vector pan (design
    holds), a reset per move (the fovea must move in steps, with a reset
    at each), or a smear (the crop stays fixed and only its size is
    gaze-driven). **Measured 2026-09-04 on the RTX 5090 with DLSS
    310.7.0: a PAN.** `dlaaCropProbe` (`src/d3d11/dlaa.cpp`, run by the
    smoke harness): a 1280×960 scene of half-pixel lines and a
    near-Nyquist grating, a 512×384 crop, 24 Halton-jittered frames per
    condition from a fresh history, error = mean distance from the
    box-filtered truth in 0..255 over the crop's interior. Still crop:
    8.76 after one frame, 2.52 after 24. Moving 4 px/frame with the
    shift in the vectors (previous minus current, the pass's convention):
    2.78. Same with the sign reversed: 22.89; with the vectors at zero:
    24.63. Saccade of 200×100 px at frame 12 with the jump in the
    vectors: 7.09 right after, 2.97 twelve frames later. The jitter's
    sign was checked in the same run: handing NVIDIA the content's shift
    on screen wins (2.52 against 4.72 for the sample offset), which is
    the convention the pass ships. Feature 6's moving fovea is therefore
    a uniform vector, and the seam question (the blend band, a headset-on
    judgement) is the one that remains.

## Phasing

1. **Render scale + sharpening** — all vendors, biggest reach, assembles
   parts that already exist. Ships first and alone, ini-tuned.
2. **The menu and the monitor** — one renderer, three clients (the menu
   driving phase 1's knobs plus the status page, the toasts, the placed
   monitor). Deliberately second: together they multiply the tuning
   velocity of everything after — the monitor shows the cost, the menu
   turns the knob, and every later phase lands as one more row instead
   of one more reason to alt-tab.
3. **Three probes before any foveation feature** (revised 2026-09-04):
   the gaze answer on the Super (item 15, built), NVIDIA's history under a
   moving crop (item 16, measured: a pan), and NVAPI's capabilities on the
   target GPUs (item 4). The first is cheapest and decides the most — whether the one
   headset with a tracker gets the eye-tracked versions at all — so it
   goes first, and the other two are desk work that needs no headset.
4. **Fixed foveation (VRS)** — NVIDIA-only, rides the census classifier;
   conservative presets; the guard-margin synergy; a row and a status
   line in the menu. Fixed centre: measurable on both field rigs.
5. **DLSS where you look, fixed centre** — feature 6 on the
   straight-ahead point, its size in degrees; the seam judged with the
   headset on. Measurable on both field rigs, and it recovers most of
   NVIDIA's cost before any tracker is involved.
6. **Eye-tracked centre** — the moving centre on 4's mask and 5's crop,
   gated on real SteamVR, a driver that answers (item 15's verdict), and
   the table route holding up. The menu gains gaze aim and dwell, and its
   highlight becomes the live check that the tracker and the mapping
   agree.

Each phase off by default, each with its own stand-down, each logging its
price. Nothing in any phase reads or writes game memory or code: render
scale and the gaze read edit answers and frames from outside (the guard's
posture exactly), foveation adds calls into NVIDIA's driver, and the menu
and monitor draw only on EDVR's own copies and watch keys they never
take. A player who wants none of it leaves the settings at their defaults
and runs a build identical in behaviour to today's.
