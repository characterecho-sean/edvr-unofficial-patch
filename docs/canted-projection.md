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

## Status

**State (2026-10-16):** handedness fix SHIPPED and FLOWN; parallel-projection
notice BUILT on `claude/canted-pp-notice`, NOT FLOWN. Elite 332841 composes raw
eye matrix `E` with head pose `S*H*S`, `S=diag(1,1,-1)`; `S*E*S` is the correct
handedness. The synthetic-cant flight confirmed the fix; the real Pimax 8KX
flight also passed with PP off. H-drop was ruled out by the synthetic-cant
flight. `GetEyeToHeadTransform` now always returns `S*E*S`, with no key. PP is
applied by the runtime; EDVR cannot undo it without the native cant and FOV.
The PP notice still needs a PP-on/PP-off flight; Pimax Play support is open.
The 2026-10-09 and 2026-10-10 entries retain the flight numbers and PP limits.

**Star dimming, 2026-10-16:** screenshots and matching show faint left-eye stars
dim on pitch with PP off: 267 still and 198 pitch pairs, median residuals
1.29/1.58 px; right/left flux moves from 0.945 still (95% CI 0.885–1.005) to
3.996 pitching (3.30–4.77). Capture 224449 is PimaxXR / Pimax Vision 8K X, PP
off, 20° between eyes, 90 Hz, 1869x1579 input, DLSS K; version unknown. All 16
frames (20376–20391) have DLSS success/history and no reset. Exact raw crops
C00/C02/C15 establish shared-world-basis error for LEFT; opposite-eye provenance
is inferred, not traced to an owner. The source fix is BUILT, NOT FLOWN. Full
build passed 26 Python gates, 164 pooled and 6 quiet rigs, production DLLs, and
installer resource validation. Receipt verifies; actual capture fixture test
passed.
That first validation build had the DLSS and FSR SDKs; XeSS SDK was absent.
The test DLL package requires a clean build with all three SDKs present.

**Capture findings:** both eyes have valid bound camera rows, but their paired
`cameraR` is identical; runtime `headR` differs slightly. The raw stars prove
that this shared world basis is wrong for LEFT. Opposite-eye provenance is
inferred from field geometry; no owner for the selected scene rows is traced.
Elite 332841 disassembly traces eye transforms to its camera setter, but not
that setter to the float-932 scene block. `headR` motion remains only 0.0037°
RMS against the large ship motion.
- **Ruled out:** TCAM D matrices as per-eye camera ground truth in run 224449,
  because they are identical across eyes and jump 90–180° between adjacent
  frames while selected S stays smooth; some D is previous-frame S. `df=9F`
  marks the same resource, not the same write or rows.
- SKY ring retained 2048 eye-frames per dump, but had 0 camera-row events and
  0 per-eye SKY lines; SKYSEC combines both eyes and cannot measure asymmetry.
  Exposure sharing added 0 fallbacks near the capture. FinalCrisp shows the
  left streak/dimming while the right image is sharp. Raw pixels now establish
  a shared-world-basis error for LEFT; implementation is built and awaits flight.
- **Next:** one PP-off DLSS K pitch flight on the same 8KX/PimaxXR setup. Confirm
  first-draw source-eye mapping and corrected left stars; inspect the basis log
  counts. FSR/XeSS flights can follow if useful. No new key or eye-folder upload.

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

(The probe flew on the legacy OpenVR proxy, which is gone; it was
reimplemented in the native OpenXR runtime on 2026-09-20, again as
`advanced.cull_guard_channel = raw | matrix` with `both` today's full
guard — now with the grown targets and the crop kept, so the eye-target
rebuild the missing cell needs still happens.)

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

## 2026-10-09 — the rotation is not dropped; it is composed in the wrong handedness

Offline disassembly of Elite build 332841 (the Epic exe the reporters run);
dumps in `analysis\decomp\verify_20261009_cant_*`. **READ** is what the
instructions say, emulated results included; **INFERRED** is what follows from
them.

**READ**

- Slots 16 (0x4E25A0) and 26 (0x4E2350) call `[rax+0x20]` (sites 0x1404E25FB
  and 0x1404E23AF), which is `GetEyeToHeadTransform`, and pass the 3x4 to
  0x4E4EF0, which reads all 12 floats.
- Slot 28 (0x4E29B0, site 0x1404E2A5C) reads only `|m[0][3]|` for an IPD
  scalar; it has one caller, 0x1428219D0. The `S*E*S` conjugation leaves
  `m[0][3]` untouched.
- The chain: per-frame job 0x2868C30, driver 0x28431D0, composer 0x283D4C0
  (which calls adapter slot +0xB0), then the eye pose times the base 4x4 with
  the full 3x3, then the camera setter 0x2878DC0. It takes forward = normalised
  row 2, right = row 1 x row 2, up = forward x right, position = row 3.
- The head conversion in 0x4E3690 (from 0x1404E38ED), emulated on three
  synthetic poses, equals `S*H*S` to about 1e-6. The slot 16 product, emulated,
  is `E @ H` to 6e-8.
- The camera stores `GetProjectionMatrix`'s 4x4 verbatim (projType 5,
  0x142878F71).

**INFERRED**

- The head pose is conjugated into game space and the eye matrix is not, so the
  composed eye rotation is wrong by `S`. For `E = Ry(theta)` that is `Ry(-theta)`
  in game space, 2 theta off per eye. Identity rotation with tz = 0 makes
  `S*E*S = E`, which is every parallel-panel headset, so nothing shows there.
- The 2026-09-12 closure assumed a dropped rotation; the field symptoms fit an
  inverted one equally well. The fold experiment and the remap arithmetic are
  measurements and still stand.

**Not found:** the cull-plane writer. The renderer's tangent extraction. The
four-tangent rebuild seen in the fold experiment is downstream of the camera
and was not traced.

**What the test build does**, so one flight separates the two hypotheses on a
headset with parallel panels:

- `advanced.simulate_cant` turns each located eye outward by the asked angle
  (left eye `orientation * Ry(+theta)`, right `Ry(-theta)`, positions
  untouched) and widens its fov to the bounding box of the turned frustum in
  the new tangent plane, right after `xrLocateViews`. Everything downstream
  sees one canted headset: the game's projection and eye-to-head answers, the
  temporal and cull-guard frusta, and the pose and fov submitted at
  `xrEndFrame`. The hidden-area mesh is withheld while it is on. The render
  size is left alone, so the pixels are not square.
- `advanced.canted_eye_fix` changes only the answer to Elite's
  `GetEyeToHeadTransform`, to `S*E*S` (negating `m[0][2]`, `m[1][2]`,
  `m[2][0]`, `m[2][1]`, `m[2][3]`). EDVR's own consumers read the located
  transform from the geometry snapshot, not from that answer.
- The runtime log carries `canted test:` lines for the cant (per eye, with the
  told and the true tangents) and `canted eyes:` lines for the correction and
  for the 3x4 actually returned to Elite with the signed yaw of each eye's
  forward axis. A log without them means the instrument never ran.

## 2026-10-09 — shipped; the test instruments are gone

The handedness fix is the behaviour: the answer to Elite's `GetEyeToHeadTransform`
is `S*E*S` for every eye on every headset, which changes nothing on parallel
panels (the matrix is returned bit for bit; `tools\openxr_native_test`
`canted_display_cases.h` pins that, and the S*E*S arithmetic against a 4x4
product) and gives a canted headset each eye's rotation in Elite's own
handedness. The section above describes the test build: `advanced.simulate_cant`,
`advanced.canted_eye_fix`, the `canted test:` and `canted eyes:` log lines and
`EdvrNativeFrameOutput` version 6 no longer exist. What a real canted headset
does with it is not yet flown.


The synthetic-cant flight's numbers (moved out of the Status block 2026-10-10):
Pimax Crystal Super via Pimax OpenXR, EDVR native runtime v0.18.3-90-gbbca39a6,
per-eye 4032x3898, true tangents l=-1.2543 r=+0.8746 t/b=±1.0293, simulated 10°
outward cant per eye (told l=-0.8828 r=+1.2425 t/b=±1.2358 for the left eye).
Runtime log `edvr_openxr_20261009_115451_577_19872.log`: fix on 17:54:54, off
17:56:45, on 17:56:50 UTC; it shows the left eye located at -10.00° and handed to
Elite as +10.00° with the fix on, -10.00° with it off. The caller census saw
`GetEyeToHeadTransform` called from exactly the three disassembled sites
(exe+0x4E25FE, 0x4E23B2, 0x4E2A5F).

## 2026-10-10 — the parallel-projection notice

`canted eyes:` lines are back in the openxr log, as a different instrument from the
test build's (which are gone, above). `parallel_projection_notice.h` decides from
three things: the system name (a canted P2 model), the angle between the two located
eye orientations (under 0.5° is parallel; 20° apart is a canted 8K X), and the runtime
name (PimaxXR is proven to honour Pimax Play's toggle). The openxr half decides once a
session in `NativeRuntimeHost::noteProjectionMode`, from the first pair that passed
`makeGeometrySnapshot`. The graphics half decides again in the menu tick from the eye
poses the native menu already holds (`nativeMenuEyePoses`) and the names the
render-settings query already carried, so no frame ABI field was added. The three
lines the openxr log can carry: `parallel projection is on` (PimaxXR; names the fix),
the same with `whether this runtime honours Pimax Play's toggle is unverified` (any
other runtime; the user is not sent to a setting that may do nothing), and
`parallel projection is off` (a canted model with canted views). A headset that is
not a canted model logs nothing. The toast and the Status hint use the existing
notice surface (`menu.toasts`; the hint slot is shared with the supersampling and
checkerboard hints, each of the three taking its turn). Rig:
`tools/openxr_native_test/parallel_projection_notice_cases.h` (names, angles at
0/0.4/0.49/0.51/0.6/20 degrees, invalid input, the verdict table, the words, the hint
slot, the host on a fixture, the wiring as source pins) and `native_menu_test` (the
eye-pose accessor).

## 2026-10-16 — reported left-eye faint-star dimming on pitch

Screenshots show left faint-star dimming during pitch; the report says
DLSS/FSR/XeSS reproduce it while AA-off/TAA is fine. Log time is close, but no
frame-to-image link exists. Its clean `v0.20.0-rc.2` tag resolves to
`f2b99bdf27d5227f2fb43df765575e1a76016f65`; `6AD165AB` is the PE link
timestamp. Rig: Pimax 8K X/PimaxXR, PP off, 20° between eyes, RTX 4090, 90 Hz,
1869x1580 input and 3738x3160 output. DLSS K active; DLSS runtime version unknown.

Projection matching (measured asymmetric intrinsics, 20° cant) found 267 still
stars, median residual 1.29 px, and 198 pitching pairs, 1.58 px. Background-
subtracted aperture flux ratio (right/left): 0.945 still (95% CI 0.885–1.005),
3.996 pitching (3.30–4.77); 99 bright both-eye pairs give 3.46. This confirms
asymmetry, not its cause.

**H-basis hypothesis:** rc.2 builds shared world-camera `worldDelta`; reusing
right-eye basis for left could explain pitch/roll; yaw is weaker because it may
commute with cant. Native per-eye `eyeDelta` is correct, but selected basis and
star rows are unknown. Alternatives: raw star/depth asymmetry or temporal
history rejection. At 20:58:44.875 camera `changed_picks=0`; at 20:58:05.552
there were 10,620 history gaps, 9,739 in burst frames, and motion-pixel counts
were unavailable. Sky ring did not run. Exposure sharing had zero 30-second
fallbacks at 20:58:22 and :52; this does not rule out every exposure effect.

First seek screenshot-session `edvr_logs\eyes\eye_*_motion.csv` and its capture
folder; the normal zip omits them. If absent, request one PP-off, DLSS K capture:
hold a space scene still, then pitch. Set `diag.sky_ring=on`; Pause
(`hotkey.dump_camera`) records TCAM history/sky buckets. During pitch, “Dump both
eyes as seen” (`hotkey.dump_eyes` if configured) saves `motion.csv` and
raw/treated frames. Collect both logs and the emitted eye folder/pool. Compare
same-eye TCAM D deltas with `cameraR` and per-eye `headR`, then raw/treated stars.
SKY is sparse; per-frame rows follow detected events only. Support H-basis only
if mismatch tracks the dark left eye; reject if `cameraR` agrees in both eyes
during pitch and raw left stars are dim. No new key or build is needed.

### 2026-10-16 capture 224449

The gfx and OpenXR logs report clean `v0.20.0-rc.2` (gfx PE build `6AD165AB`).
`--expect-build HEAD` accepts the longer descendant version at version level;
this is not an exact HEAD build. OpenXR confirms PimaxXR,
Pimax Vision 8K X, PP off, 20° between eyes, 90 Hz; input is 1869x1579 and output
3738x3158. All 16 eye-metadata frames 20376–20391 report DLSS success/history,
no reset.

Pause dumps at 22:44:50.051 and 22:44:58.627 each cover 4096 eye calls and
report 0 camera-row events in 2048 frames. TCAM is available for both eyes in
the latest 16 frames; `df=9F` means same resource, not same write or rows. The
TCAM D matrices are identical across eyes and jump 90–180° between adjacent
frames while selected S stays smooth; some D equals the previous-frame S.
- **Ruled out:** TCAM D matrices as per-eye camera ground truth in run 224449,
  because they are identical across eyes and jump 90–180° between adjacent
  frames while selected S stays smooth; some D is previous-frame S.

`cameraR` reconstructed from S agrees within 1e-7, proving the field is shared.
The raw-star fits below prove this shared world basis is wrong for LEFT.
Runtime `headR` has the correct 20° conjugation, but head turn is only 0.0037°
RMS against large ship motion. SKY rings retain 2048 eye-frames each, with 0
events and 0 per-eye SKY lines; SKYSEC combines eyes. Its final-bin motion rises
to 73–79, while raw/out remains about 0.92. Exposure sharing adds 0 fallbacks
near capture (2 cumulative, both earlier). FinalCrisp shows visible left
streak/dimming against a sharp right image.

The provenance audit found no eye assignment for the selected scene rows.
Elite 332841 disassembly traces the eye transform to the camera setter, but
does not trace the setter to the float-932 scene block. Draw-time per-eye
constants are separate evidence. Opposite-eye provenance remains an inference
from field geometry, not a traced owner.

The exact raw left crops `eye_224449_C00.bmp`, `eye_224449_C02.bmp`, and
`eye_224449_C15.bmp` were supplied and verified as 1400px crops from the
1869x1579 input, origin 23489. Their motion fit is recorded next.

### 2026-10-16 — raw-pixel star motion fit

On Pimax Vision 8K X / PimaxXR, PP off, 20° eye cant, 90 Hz, DLSS K (runtime
version unknown; tiny head motion), C00↔C02 gives 137 free-homography inliers.
With shared world `cameraR`, residual is median 5.045 / RMS 5.123 input pixels;
applying Y(-20) to the world and Y(+20) to the view gives 0.326 / 0.359. Free H
gives 0.312px median; free Kabsch polar-rotation gap is 0.487° for shared vs
0.0043° corrected. Fixed-yaw bootstrap 95% CI is -20.20° to -19.87° for the
Y(-20) world / Y(+20) view definition.

C15 was matched image-only: a two-star similarity seed, 34 central-tile
projective RANSAC seeds, then 123 full-sky inliers. C15↔C02 all-candidate 1px
counts are 120 for selected -20° and 0 for shared or +20°; on 123 matched
stars, shared residual is 28.211 / 30.233px median/RMS vs 0.318 / 0.416px
corrected. Head-center ±10 candidates are worse. Frustum XY-shift optimization
remains 2.48 / 14.64px RMS vs 0.36 / 0.42px corrected; ≤1px jitter cannot
explain the measured displacement. **Ruled out:** unchanged frustum-center or
jitter-offset error as explanation of measured star displacement, because its
optimized residuals remain far above the corrected shared-world fit.

The result establishes a shared-world-basis error for LEFT from field
geometry; opposite-eye provenance is inferred, not traced to an owner. The
fix tags only the exact DSV source eye and converts the full selected pose from
known source E to each E with handedness, preserving original motion,
unknown/carry behavior, and the parallel-projection bit. It is built, not flown.
The temporary basis instrument logs `temporal canted world 30s:` with
`left_examined`, `right_examined`, `left_converted`, `right_converted`,
`right_to_left`, `left_to_right`, `geometry`, `mapped`, `unknown_eye`, and
`source_switch`. First-draw DSV mapping still needs in-headset confirmation.
Next flight: same 8KX/PimaxXR, PP-off DLSS K pitch. Pass requires
`left_converted` and `right_to_left` above zero with no left dimming/streaking.
Collect logs only initially; no large eye-folder upload. The basis instrument
is temporary while this arc remains open; no new config key.
