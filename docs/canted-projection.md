# Canted displays, and what parallel projection actually costs

Pimax headsets angle their two panels outward. Elite requires those users to
turn **parallel projection** on, and it costs them roughly a third of their
frame rate. This document records what that setting actually does — measured
from outside the game, to four decimal places — what the game does wrong
without it, and the shape of the fix that follows.

Everything below was measured through EDVR's `openvr_api.dll` proxy, which
records what the game asks the VR runtime. No game file, game memory or game
code is touched at any point.

## What OpenVR says about a canted headset

A canted headset is described in two separate places, and this split is the
whole story:

- **`GetProjectionRaw`** gives each eye's frustum *in that eye's own frame* —
  four tangents, which can describe any asymmetry but cannot describe a
  rotation.
- **`GetEyeToHeadTransform`** gives each eye's placement — half an IPD of
  translation, **and a 3×3 rotation**, which on a canted headset is the cant.

A game that uses only the translation renders each eye's frustum aimed
straight ahead instead of angled outward. Each eye is then internally
correct and the two disagree with each other, by exactly the cant.

**Asymmetry is not the problem.** Elite renders strongly asymmetric frusta
correctly today: the Quest 3 reports 54° outward against 40° inward
horizontally, and is rendered right. Whatever parallel projection is
compensating for, it is not the shape of the frustum.

## What was measured

A Pimax 8KX owner ran the test card twice, changing nothing but the parallel
projection checkbox (Frontier issue tracker aside, this is
[issue 24](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/24);
rig and full numbers at the end).

**The cant reading, which is the whole question:**

| | left | right |
|---|---|---|
| PP **on** | 0.00° | 0.00° |
| PP **off** | 10.00° | 10.00° |

The log prints the angle's magnitude, so both eyes positive is expected;
~10° per panel matches the 8KX's geometry. **The runtime does report a real
cant, and EDVR can see it.** Parallel projection is what zeroes it.

**And what the game does with it, from the same session:** both eyes correct
*alone*; a **rotational** mismatch between them that would not fuse; fixed
relative to the head wherever the player looked; growing toward the outer
edges. A desktop mirror capture of one eye shows a geometrically sane image —
straight edges straight, HUD layer normal.

Every one of those is the signature of a dropped `GetEyeToHeadTransform`
rotation, and nothing else fits. A mangled projection would show in one eye
alone; a convergence or IPD error would displace the images sideways, not
rotate them; an error in the view *pose* would move as the head moved. The
error growing toward the edges is confirmatory rather than complicating: a
*constant* 10° angular error becomes a growing pixel disparity through the
tangent mapping.

## What parallel projection is, exactly

The tangents from the two runs, left eye:

| | outer (l) | inner (r) | vertical (t/b) | totals |
|---|---|---|---|---|
| PP **on** | −2.7692 → 70.14° | +0.8878 → 41.58° | ±1.5867 → 57.8° | 111.7° × 115.6° |
| PP **off** | −1.7422 → 60.14° | +1.2616 → 51.61° | ±1.2698 → 51.8° | 111.7° × 103.6° |

Read the horizontal: **60.14 + 10 = 70.14, and 51.61 − 10 = 41.61.** Not
approximately. Running the PP-off tangents through the tangent-addition
formula with tan 10°,

    X = (x + tanθ) / (1 − x·tanθ)

reproduces the PP-on pair to every digit the log prints: −1.7422 → −2.76932
against a logged −2.7692, and +1.2616 → +0.88778 against +0.8878. The
horizontal *total* is identical at 111.7° in both runs, because it is the
same cone measured from a different axis.

So parallel projection is, precisely:

1. **Rotate each eye's frustum by the cant, back into head space.** This is
   an exact angular rotation and loses nothing.
2. **Bound the result with an axis-aligned frustum.** A rotated frustum is a
   *trapezoid* in the new tangent space, so an axis-aligned box around it must
   be bigger. That is why the vertical grows, 51.8° → 57.8°, on a rotation
   that is purely horizontal.

Step 2 is the entire cost. Step 1 is free.

*(Inference, not measurement: fully bounding the rotated frustum's outer
corners would need the vertical tangent at ×1.47, and the runtime uses ×1.25 —
so parallel projection appears to accept clipping the extreme outer-top and
-bottom corners, presumably where the lens shows nothing anyway.)*

## What it costs

**52% more pixels per eye.** Three independent measurements from the same two
sessions, all landing on the same ratio:

| | PP on | PP off | ratio |
|---|---|---|---|
| Recommended render target | 4550×3948 | 3738×3160 | 1.5208 |
| Actually submitted | 2957×2566 | 2429×2054 | 1.5208 |
| Tangent-space area | 3.6570 × 3.1734 | 3.0038 × 2.5396 | 1.5212 |

Turning parallel projection off returns **34% of the pixels**. The runtime
sizes its recommended target in exact proportion to the tangent-space area,
which is why the three agree.

Frame time moves less than that, and should: GPU frame time went 10.5 → 9.4 ms
docked (10%), 8.3 → 6.4 in flight over a planet (23%), 7.4 → 5.7 in space
(23%). The gap between 34% of the pixels and 23% of the frame is the part of
the frame that is not fill-bound. The saving is largest wherever the rig is
pixel-bound — higher FOV settings, higher HMD Quality, and the wide-FOV modes
this rig was not tested at.

## What it is not

- **Not a symmetrization.** With parallel projection *on* the frustum is still
  strongly asymmetric — 70.1° outward against 41.6° inward. It is a rotation
  plus a bounding box, which is a different trick from the cull guard's
  symmetrize-and-crop.
- **Not something EDVR can turn on or off.** Parallel projection happens
  inside the Pimax runtime, upstream of EDVR's seam. EDVR sees the difference
  in what gets *reported*; it cannot reach the setting. Anything EDVR does
  here only matters once the user has turned it off themselves.
- **Not an Elite-only setting.** It is a runtime workaround for any title that
  drops the eye rotation. This document says nothing about the other titles.

## What a fix inside the game would look like

Use the whole of `GetEyeToHeadTransform` — the 3×3 as well as the translation
— when building each eye's view matrix. The game already queries that call
about a dozen times a frame. Every Pimax owner would then turn parallel
projection off and get 34% of their pixels back, and nothing else in the
renderer would need to change.

## What EDVR could do about it

The rotation does not have to arrive through the channel the game ignores.
`P · Reᵀ` is the same result, and a 4×4 can carry a rotation where four
tangent floats mathematically cannot. EDVR already rewrites that 4×4 per eye,
per frame, in `MatrixReceiver::GetProjectionMatrix` (`src/openvr/system_hook.cpp`)
— the cull guard widens it and the temporal pass shifts it sub-pixel, and the
rendered image demonstrably follows.

The design the numbers point at:

> **The bounded tangents to the culler, the canted matrix to the rasterizer.**

Fold `Reᵀ` into the 4×4 so the game rasterizes the *true* canted frustum, and
report the wider bounded tangents — parallel projection's own numbers —
through the raw thunk so the culler over-covers. A generous culler costs no
fill, so the wide half is free. And because the runtime already recommends the
smaller target with parallel projection off, the pixel saving arrives without
touching `GetRecommendedRenderTargetSize` at all.

### The gates, before anyone builds it

1. **Separability, which is phase 0.** Nobody has yet shown that the culler
   reads the raw tangents while the renderer reads the matrix. The cull guard
   always edited both together, deliberately, so it cannot tell them apart.
   The experiment is to edit one channel only and watch whether the terrain
   tiles move or the image does.
2. **This would be the first deliberate disagreement between the two
   channels.** `system_hook.cpp` states the opposite as policy — *"the game
   must never see mixed answers within one frame"* — and the whole go-live
   discipline is built around it. The canted fix needs them to differ on
   purpose, which is a real departure and should be a conscious one.
3. **Every pass must render through the runtime's matrix.** The frame census
   shows several — the scene at ~300 draws an eye, a separate cockpit layer,
   shadow maps. Any pass building its own projection from the tangents would
   stay parallel while the main pass turns, and the layers would disagree.
4. **Depth reconstruction.** A rotation makes clip-space depth depend on x and
   y, so anything inverting depth with the canonical formula needs the
   composition — Elite's own HBAO, and EDVR's temporal reprojection. That
   composition is already written for canted panels in
   `src/openvr/temporal_aa.cpp`, and has been a no-op on every flight so far
   because neither of this project's headsets is canted.
5. **Native SteamVR is untested.** The measurement above came through
   OpenComposite → PimaxXR.

## Reproducing the measurement

Any canted headset, EDVR v0.14.1 or later with both files installed, and:

    [fix]
    temporal_aa = on      ; this is what prints the cant
    cull_guard  = off     ; the other thing that lies about the projection

    [advanced]
    observe_projection = 1   ; on by default — but it gates the whole
                             ; system hook, so 0 silences the cant too

Fly the same scenes twice, changing only the runtime's parallel projection
setting, and read four lines out of `edvr_logs\*_vr_*.log`: `the eyes sit`
(the cant), `TRUE tangents` (both eyes), `GetRecommendedRenderTargetSize`, and
`ONE EYE is` (what the game actually submitted). The full protocol, including
the visual diagnostics that separate a dropped rotation from a mangled
projection, is the test card:
https://claude.ai/code/artifact/08be1748-b567-40fb-beae-78f11fcd31dc

Note that the system hook installs only on `IVRSystem_012` and refuses loudly
on anything else, so a runtime presenting a different generation produces a
log with no `IVRSystem` lines at all — and that version string is worth
reporting on its own.

## The field data

Pimax 8KX, FOV Normal, RTX 4090, 90 Hz, Elite HMD Quality 0.5, OpenComposite →
PimaxXR (OpenXR Toolkit 1.3.2), EDVR v0.14.1 (build 6A9F3230), 2026-09-08.
Headset FOV signature `112x116`. IPD ±0.0301 m, identical in both runs. Both
sessions clean — no `INERT`, no refusal, no stand-down.

With thanks to the owner, who ran it: neither of this project's own headsets
has canted panels, and both report exactly 0.00°, so every cant-aware line in
EDVR had never once been exercised against a real angle before this.
