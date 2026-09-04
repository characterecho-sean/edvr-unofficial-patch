# Shimmer on steady geometry (VR): the render pose never rests, and sub-pixel detail is sampled as dashes

**Type:** Bug report — VR rendering
**Area:** Whole frame; most visible on the main-menu ship's hull lines, HUD hairlines and text
**Game version:** 4.4.1.0, build r332753 (long-standing; reproduced on every 2026 build tested)
**Severity:** Cosmetic per frame, but it is *the* shimmer VR players report — on every screen, every session. Two mechanisms account for most of it; both are engine-side, and the first is cheap to fix.

---

## Summary

Hold your head still at the main menu and look at a near-horizontal panel
seam on the ship. It flickers continuously. Roll your head about 25 degrees
either way and it stops; level again and it resumes. Two separate things
produce this, and we measured both on stock installations:

1. **The pose the game renders from never rests.** With the headset lying
   on a desk, the runtime's reported *position* holds to under 0.05 mm per
   frame, but its reported *orientation* wanders about 0.1 arcminute every
   frame, with a step above 0.6 arcminutes (about half a rendered pixel at
   the centre of a current high-density headset) roughly once a second.
   The game renders every frame from that pose, so any feature about a
   pixel wide — a hull seam, a HUD hairline, a text stroke — crosses pixel
   rows frame to frame and blinks. The compositor then re-warps every
   frame by the same motion, so the wander is applied twice. Nothing
   downstream can remove it: post-process AA is spatial, and supersampling
   with any resolve kernel passes a *moving* line faithfully. Holding the
   render pose and telling the compositor the frame's display pose stopped
   the shimmer outright.

2. **Sub-pixel detail is sampled as dashes.** With the pose held still,
   the same seam shows as a row of bright dashes with dark gaps. The dashes
   get finer and more frequent as HMD Quality rises and merge when the
   head rolls: a feature narrower than a render pixel, sampled once per
   pixel, lit wherever the sample happens to land on it. We tested the
   geometry explanation directly — every solid-fill rasteriser state the
   game creates rewritten with Direct3D 11.3 conservative rasterisation,
   which lights every pixel a triangle touches — and the dashes were
   unchanged while other surfaces grew triangle artifacts. So the seam is
   not a geometry sliver: it is texture-space detail, most likely a
   normal-map ridge whose specular highlight fires only where the sample
   lands on the ridge, or a one-texel bright line, sampled at the render
   grid's phase. The tracker's wander used to slide that pattern along the
   seam every frame; that was the shimmer. Nothing after shading can
   recover what a single sample per pixel skipped. Only more samples per
   pixel, or band-limiting the detail and its specular before it is
   sampled, can draw it whole.

The rest of this report gives the reproduction, the measurements, what
was excluded and how, and fix directions for each mechanism.

## Steps to reproduce

1. Stock installation, no mods, VR through SteamVR. HMD Quality 1.0,
   anti-aliasing Off or SMAA (it makes no difference).
2. Main menu, the ship in the hangar. Keep the head still and look at a
   bright near-horizontal seam on the hull, about a pixel wide.
3. Observe the seam shimmering. Roll the head about 25 degrees either
   way: it stops. Level: it resumes.
4. Put the headset on a desk and watch the desktop mirror window, which
   shows the left eye as rendered, before the compositor: the seam still
   shimmers there.
5. Log the HMD pose returned by `WaitGetPoses` every frame with the
   headset on the desk: position steady, orientation wandering about 0.1
   arcminute a frame.
6. Hold the render pose constant (we did it in an OpenVR proxy, replacing
   the HMD entry `WaitGetPoses` returns): the mirror steadies, the headset
   does not. Also submit each frame with `Submit_TextureWithPose` carrying
   the HMD pose predicted to that frame's photon time: the headset
   steadies too. The seam now shows static dashes. Raise HMD Quality to
   1.5: the dashes become finer and more frequent.

**Expected:** a steady ship shows steady lines.

**Observed:** as above, on every build tested.

Reproduced on two stacks, stock:

- Pimax Crystal Super, native SteamVR over DisplayPort — all measurements
  below, and the roll test.
- Meta Quest 3 over Virtual Desktop (SteamVR) — the same shimmer at rest,
  so streaming compression is not the cause.

Host: Windows 11 Pro, GeForce RTX 5090.

## Measurements

The pose the runtime reports, headset lying on a desk, two captures of
1200 frames each at 90 Hz (Pimax Crystal Super, native SteamVR; the
capture is the HMD `TrackedDevicePose_t` from `WaitGetPoses`, per frame):

| Quantity | Reading |
|---|---|
| Position change per frame | under 0.05 mm, every frame |
| Orientation change per frame, mean | 0.11 to 0.14 arcmin |
| Largest one-frame turn | 0.46 arcmin (capture 1), 1.63 arcmin (capture 2) |
| Frames turning more than 0.3 arcmin | 95 of 2400 |
| Frames turning more than 0.6 arcmin | 26 of 2400 |

For scale: at the centre of the Crystal Super's field, one rendered
pixel at SteamVR's recommended size is on the order of one arcminute. A
tenth of a pixel every frame, with a half-pixel step once a second, is
enough to walk a one-pixel line across a row boundary continuously.

The orientation figures are computed from the antisymmetric part of the
relative rotation between consecutive frames, not from the trace: near
identity the trace route reads float32 rounding of the pose matrix as
about two arcminutes of phantom turn, which is worth knowing for anyone
repeating the measurement.

**The hold experiment.** With the render pose held constant, the seam
steadied in the mirror window and kept blinking in the headset. Adding
`Submit_TextureWithPose` to every submit, carrying the HMD pose predicted
to the frame's photon time (`GetTimeSinceLastVsync`, the display frequency
and vsync-to-photons properties, `GetDeviceToAbsoluteTrackingPose` in the
compositor's tracking space, fetched at submit — 17.8 ms ahead at 90 Hz on
this rig), the seam steadied in the headset as well: 740 consecutive
submits carried a pose, none fell back, and the shimmer stopped. The
compositor had been re-warping the held frame by the tracker's motion;
told the display pose, it had nothing left to re-warp.

**What the hold exposed.** With the view still, the seam showed a row of
bright dashes with dark gaps — fully on or fully off, not a soft
modulation. At HMD Quality 1.5 the dashes were finer and more frequent
than at 1.0. Rolling the head merged them. A resolve kernel at the door
(Mitchell at radius 2 px, then a Gaussian at radius 2 px, σ = 1 px, both
on the 1.5x frame) left them unchanged: a dash every 20 to 60 pixels is a
low-frequency pattern along the line, and what a single sample per pixel
skipped is not in the image to recover.

**The geometry test.** To separate a sub-pixel triangle from sub-pixel
texture detail, every solid-fill rasteriser state the game created was
rewritten with conservative rasterisation (Direct3D 11.3, hardware tier 3;
eight states, back-, front- and no-cull alike, eight wireframe states left
alone). Under it a triangle lights every pixel it touches, so a geometry
sliver would have come out as a solid line. The dashes did not change,
and other surfaces showed triangle-shaped artifacts from the extrapolated
attributes, so the seam is not a geometry sliver and this is not a fix.

## What was excluded, and how

| Suspect | Excluded by |
|---|---|
| Streaming compression | the same shimmer on DisplayPort |
| Lighting or shader animation | indifferent to head roll; the shimmer is not |
| The compositor, the panel, the lenses | the mirror window shows it, headset on a desk |
| The player's own head motion | headset on a desk |
| The supersample resolve kernel | unchanged by Mitchell 2.0 and Gaussian 2.0 at 1.5x |
| Post-process AA | the same with Off and with SMAA |
| Tracking position noise | steady to under 0.05 mm per frame |
| A sub-pixel geometry sliver | conservative rasterisation on every solid state: dashes unchanged |

What remains is the reported orientation, and the hold experiment
confirms it: remove that motion and the shimmer stops.

## Why the engine's own anti-aliasing cannot help

FXAA, SMAA and MLAA run on the finished image and blend along edges they
find in it. Mechanism 1 is temporal — the image is right every frame and
wrong between frames — so a spatial filter turns flicker into smoothly
blended flicker. Mechanism 2 is a single sample per pixel landing on or off a
feature thinner than the pixel — and a dashed line is not an edge pattern
those filters recognise. Neither can be reached from the post chain. The
field's lore that "only HMD Quality does anything" is right for the wrong
reason: supersampling helps mechanism 2 by putting more samples on the
feature, and does nothing for mechanism 1.

## Suggested fix directions

### For the wandering render pose — small, contained in the VR pose path

This is the one to do first: a few dozen lines in the code that fetches
the HMD pose and submits the frame, no cost on the GPU, and it removes the
shimmer players see most, on text and menus especially, because the head
is still exactly when people are reading.

1. **Filter the render pose adaptively.** Track the head's speed as the
   turn since the last frame plus its travel scaled to a couple of metres
   (a millimetre of travel counts as about 1.7 arcminutes), smoothed over
   about five frames. Let the render pose move toward the tracked pose by
   a factor *k* each frame: 0.02 at and under 0.3 arcminutes a frame
   (a half-second time constant that passes the tracker's slow wander and
   cuts its jitter fifty times), 1 at and over 1.5, linear between. That
   is a One-Euro-style filter with the cutoff tied to speed; the world is
   stock the moment the head moves, and while still it sticks to the head
   by under a pixel, well below the eye's own fixational jitter.

2. **Tell the compositor which pose to display the frame at.** Every VR
   runtime re-warps each frame from the pose it believes the frame was
   rendered from to the pose it predicts at display. Give it, while
   still, the pose predicted for the frame's own photon time, fetched at
   submit, so the warp is near zero and the held frame reaches the panel
   as rendered; in motion, give it the true render pose so late-warp
   latency compensation stays. Blend the two with the same *k*. In OpenVR
   this is `Submit_TextureWithPose` with a `VRTextureWithPose_t`; SteamVR
   keeps only the later of the two eyes' poses, so both eyes must be told
   the same one. In OpenXR it is the view pose in
   `XrCompositionLayerProjectionView`; in the Oculus SDK, the layer's
   `RenderPose`.

3. **Snap, never filter, a discontinuity.** A recentre or a tracking
   glitch is a jump the head could not have made while the speed estimate
   is low; follow it at once rather than smoothing it.

We ship exactly this in a community proxy (`shimmer_rest`, public source,
`src/openvr/compositor_hook.cpp` and `src/common/rest_math.h` in the
repository below), field-verified at the main menu on the Crystal Super:
with the head still the seam and the menu text stand steady, and turning
the head shows no swim or lag. The numbers above are the ones it runs.

### For sub-pixel detail — the industry answer

1. **Temporal anti-aliasing with a velocity buffer** (TAA, DLAA, FSR 2/3,
   XeSS). The deferred renderer already has the depth and matrices; a
   per-pixel motion vector pass is the missing input. Sub-pixel jitter
   accumulated over a handful of frames integrates a feature's true
   appearance into a continuous line, and it is the answer every other
   deferred engine settled on. The rest-pose filter above makes the
   history exactly registered while the head is still, which is when the
   shimmer is worst, so the two fixes compound.

2. **Band-limit the detail before it is sampled.** Specular anti-aliasing
   (Toksvig or geometric) so a normal-map ridge's highlight widens with
   distance instead of vanishing between samples; variance-preserving mip
   filtering of the normal maps; and a texture LOD bias toward the softer
   level for the finest hull detail. For genuinely sub-pixel geometry, a
   minimum screen-space width. MSAA is impractical in the deferred path,
   and supersampling is too expensive at the resolutions VR needs.

3. **Not conservative rasterisation.** Tested: no effect on the seam, and
   triangle-shaped artifacts on other surfaces from the extrapolated
   attributes.

## Impact note

This is the shimmer behind the long-running "no anti-aliasing in VR" and
"supersample to fix it" threads. Mechanism 1 is present on every screen
the moment a player holds still to read something, and it is a small,
self-contained change with a large visible payoff. Mechanism 2 is the
larger project, and the one that finally lets VR players turn HMD Quality
down.

---

*Report prepared from renderer-level and pose-level instrumentation done
for the EDVR community patch project
(https://github.com/characterecho-sean/edvr-unofficial-patch; the design
notes are in docs/anti-aliasing.md). All measurements above were confirmed
on stock, unmodified installations. Happy to provide the pose captures,
the instrument builds, or to run diagnostic builds.*
