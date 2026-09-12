# Canted displays, and what parallel projection actually costs

Pimax headsets angle their two panels outward. Elite requires those users to
turn **parallel projection** on, and it costs them roughly a third of their
frame rate. This document records what that setting actually does — measured
from outside the game, to four decimal places — what the game does wrong
without it, what was tried, and why the cost turns out to be **irreducible
from outside the game**.

**The conclusion, up front (2026-09-12):** parallel projection is close to
the cheapest possible way to drive a canted headset from this game, because
Elite rebuilds its projection from four tangent numbers and a rotation cannot
pass through four numbers. Every route that avoids the cost has to make the
game render in the panel's own rotated frame, and nothing the runtime can
say to the game achieves that. Four field flights and one built-and-flown
fix established this; the reasoning is in *What was tried*, so it is not
re-derived.

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

## What was tried, and why the cost is irreducible

The measurements above suggested a fix: the rotation does not have to arrive
through the channel the game ignores. `P · Reᵀ` is the same result as applying
the eye's rotation in the view, and a 4×4 can carry a rotation where four
tangent floats cannot. EDVR already rewrites that 4×4 per eye, per frame, in
`MatrixReceiver::GetProjectionMatrix`, and the cull guard's edits to it
demonstrably reach the rendered image. Three experiments followed, in order,
and each closed a door.

### 1. The separability probe — which call does the culler read?

`advanced.cull_guard_channel = raw | matrix` lies through one projection call
and leaves the other true, touching nothing on the image path: no wider
targets, no crop. Whatever moves is what that one call feeds. Flown on the
8KX, parked on Shinrarta Dezhra A1 (2026-09-09), with `both` as the control:

| channel | lied to | terrain quads | the picture |
|---|---|---|---|
| `raw` | tangents only | no change | unchanged |
| `matrix` | matrix only | no change | **visibly widened** |
| `both` | both, plus wider targets and the crop | **covered** | unchanged, as designed |

Two things were learned. **The rasterizer follows the matrix and ignores the
tangents** — the only channel that moved the picture was the matrix. And
**the culler follows neither call alone**; only the full guard, which changes
both answers *and* the render target size, moved the quads. So there is no
way to widen what the culler sees without also widening what is rendered:
the idea of a cull guard that costs no pixels is dead, and stays dead.

(The `both` control differs from the split channels in three ways, not one,
so "the culler wants both calls to agree" and "the culler follows the render
target size" were never separated. It does not change either conclusion.)

The probe also found, as a side effect, that the guard's stage-1 adoption
test was keyed to the runtime's recommendation rather than to what the game
actually submits, so it could never go live on any rig with HMD Quality below
about 1.0. Fixed the same day (`sizeAdopted`); see the cull guard notes.

### 2. The fold — `P · Reᵀ` handed to the game

Built as `fix.canted_projection` (v0.14.1-49-g21b7d5f, reverted in
8c26729, the code remains in history). Rotations fetched at a frame boundary
and cached; the fold applied last in the receiver, over the true, widened or
jittered frustum; six arithmetic cells around the sign, the mirror between
eyes, and the identity on parallel panels.

Flown on the 8KX with parallel projection off (2026-09-10). The plumbing
worked — `canted projection LIVE: the eyes sit 10.00 and 10.00 degrees` — and
the result was **a perfectly rendered, undistorted cockpit against a black
world, with the two eyes horizontally offset from each other.** Not the
predicted edge trimming. Everything beyond the canopy was gone.

The explanation fits every earlier observation: **Elite does not render
through the runtime's matrix. It reads the four tangent-carrying elements
out of it and builds its own projection** — which is why the game's real
render projection is infinite-far reversed-Z when the runtime's is not, and
why the `matrix` channel widened the picture (it changed the *extracted*
tangents, not the matrix the game drew with). A rotation lives in
`m01 m10 m20 m21 m30 m31`, every one of which is zero in every projection the
game has ever seen, and none of which survive a tangent extraction. What
survives is a *corrupted* tangent pair: on the 8KX's numbers the true
`l = −1.7422, r = +1.2616` extracts as roughly `l ≈ −2.03, r ≈ +1.16` —
shifted a third of the way the cant needs, and 6% wider. That is the
horizontal offset, exactly.

So the matrix channel is the same wall as the raw one. Four numbers get
through. A rotation is not four numbers.

### 3. The remap — do what parallel projection does, but cheaper

Parallel projection fixes the stereo entirely through the tangents, which is
*why* it works on a game that ignores the eye rotation. Its 52% splits into
two costs that can be measured separately from the 8KX's tangents:

| | horizontal tangent span | cost |
|---|---|---|
| the true canted frustum | 3.0038 | — |
| PP's horizontal remap into head space | 3.657 | ×1.217 |
| PP's vertical padding | ×1.25 | ×1.52 in all |

The remap is an exact rotation re-expressed as a frustum (the
tangent-addition formula reproduces PP's numbers to every printed digit).
The padding looked discretionary — so the plan was to report the remapped
tangents ourselves, skip the padding, and warp the head-space render into
the eye's frame at submit, for a cost of ×1.217 instead of ×1.52.

Two things kill it. First, the warp is not optional either: with parallel
projection off the runtime displays each submitted image straight onto its
panel, expecting the eye's frame, so a head-space render lands 10° wrong
per eye — the original divergence, reproduced. Second, and decisively,
**the vertical padding is not discretionary.** A rotated frustum sampled
from a head-space render needs source pixels above and below the rendered
rectangle by a factor of `1 / (cos θ · (1 − x·tan θ))`, which grows toward
the outer edge. On the 8KX:

| column of the eye | factor | black at top and bottom with no padding |
|---|---|---|
| centre | 1.015 | ~2% |
| halfway out | 1.113 | ~10% |
| three-quarters out | 1.232 | ~19% |
| the outer edge | 1.466 | ~32% |

Without padding that is a black wedge along the top and bottom of the entire
outer half of each eye. PP's own ×1.25 already leaves ~15% black at the
extreme corners, which the lens evidently hides; anything less lands in view.
Restore enough padding to avoid it and the cost is ×1.217 × 1.25 = **PP,
exactly.** There is no middle that saves anything visible.

### Why nothing else works either

Every route that saves pixels has to make the game render in the panel's own
rotated frame, so that the panel needs no covering from head space. The game
will not do that from the eye transform (it discards the rotation), cannot be
made to through the projection (only tangents survive), and cannot be told to
through the pose (both eyes share one head pose and need opposite rotations).
An OpenXR runtime with per-view pose and field of view could *submit* a
canted view correctly, but the game would still have rendered head-space,
which still has to cover the panel, which is the same padding and the same
cost.

The one route left is rewriting the game's per-eye view matrices in its own
constant buffers from the D3D11 side — per draw, in every pass, with the
CPU-side culler and the interface not following. That is the "too deep in
the pipeline" answer the original question anticipated, and it is the
correct one.

### What the workstream produced instead

- **The diagnosis**, which is complete and stands: Elite drops the eye
  rotation, parallel projection is an exact rotation plus a bounding box,
  and the box costs 52% per eye. That belongs on Frontier's tracker.
- **The separability probe** stays in the build as a permanent diagnostic
  (`advanced.cull_guard_channel`).
- **The adoption fix**, a shipped-feature bug affecting anyone below HMD
  Quality 1.0, found from a stall line most people would have scrolled past.
- **A field method** — the parked spot, the same head sweep, one screenshot
  per setting — that turned "practically random" terrain quads into a
  readable instrument.

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

The three later flights (2026-09-09 probe, 2026-09-09 probe rerun with the
adoption fix, 2026-09-10 fold) were the same rig under **native SteamVR**
(`launch centre: the runtime under this proxy reads as SteamVR (Valve's
own)`), at FOV Wide with parallel projection on for the probes — signature
`122x104`, tangents `l=-2.7692 r=+1.2616 t/b=±1.2698`, recommended
5016×3160 — and parallel projection off for the fold. Logs and screenshots
for every flight are attached to issue 24.

With thanks to the owner, who ran all four: neither of this project's own
headsets has canted panels, and both report exactly 0.00°, so every
cant-aware line in EDVR had never once been exercised against a real angle
before this — and the fold could only ever have been tested in his cockpit.
The answer is a negative one, and it is his.
