# Black holes: a shadow, a photon ring, and the whole sky bent round them

Sean, 2026-09-30: Elite's black holes are blurry, warped blobs. There is no
shadow, the lensed region is soft, and nothing about them carries the awe the
real thing would. This arc replaces the blob with what general relativity says
a static observer sees near a non-rotating hole, and gives a feeding hole the
thin accretion disk it would have. The physics is computed exactly, offline, by
`tools\blackhole_optics.py` (gated in `build.bat`), so the shader is held to
numbers rather than to taste; this doc owns the plan, the evidence, and what is
ruled out.

## Status

State, 2026-09-30: opened. No flight flown, nothing in the game identified, no
rendering code written. Done: the physics, as a tool with a self-test in the
build gate, and previews of the shader's own algorithm.

- **First target, Sean 2026-09-30: Sagittarius A* alone**, majestic and as
  physically accurate as current knowledge allows. Its design, with rendered
  images: `docs\design-sagittarius-a-2026-09-30.md`. Flight 1 below still comes
  first and still flies at HIP 63835: it identifies the draw, and the design
  develops there with Sgr A* posed on the nearby hole, so only the last flight
  goes 25,900 ly. The stellar holes follow on the same machinery.
- Goal: in place of the blob, the shadow at its true size for the distance, the
  photon ring, the whole sky lensed with its secondary image, correct in each
  eye; for feeding holes, a thin disk with its Doppler and gravitational
  shifts. Sizes that will be checked in flight: "What right looks like".
- Reference: `tools\blackhole_optics.py` (`--at MASS KM`, `--render`,
  `--self-test`). The shader's march is the tool's `march`, held to 1e-5 rad
  against an independent quadrature and published limits; a float32 mirror
  stays within 1.3e-5 rad out to the secondary image.
- Environment the fix will depend on: Elite's galaxy-background cube size
  (Environment / galaxy quality, 1024 to 4096 per face, or overridden), the
  per-eye render size, and game build (the 30 June 2026 background rework). A
  draw replacement keyed on the game's own shaders should not depend on the VR
  runtime; to be confirmed on the Oculus route before any claim.
- Open, blocking everything: how Elite draws the lens, M1 (sky cube re-sampled,
  the community's evidence points here) to M4; why it is blurry, B1 to B4.
  Signatures for each: "Hypotheses".
- Next flight: flight 1, identification. No build needed; settings and steps in
  "Flight 1". Its optional step 5 also gets the lens draw's constants.
- Ruled out: none yet.
- Choices waiting on Sean (none blocks flight 1): which holes get disks, the
  disk's temperature, key names. Recommendations in "Choices".
- After flight 1: Plan, phase 2.

## Plan

1. **Identify** (flight 1). The lens draw, its shaders' bytecode, what it
   samples and at what size, its place in the eye sequence, B1 to B4.
2. **Read it.** Disassemble; decode which constants carry the hole's position
   and size, and calibrate their units against HUD distances (flight 1 step 5,
   else the first test flight of phase 3 carries a probe for it). Eye origin
   and view-projection are already known in the scene constants (VS/PS b1,
   registers 270 to 275).
3. **Replace.** A verdict rung keyed on the identified shaders and shape
   (night_vision.cpp is the template) swaps in EDVR's pixel shader at the same
   point in the frame: the march per pixel, sampling the sky source flight 1
   names. HLSL built at build time (tools\temporal_shader_build). Its WARP rig
   renders into a cube whose texels encode their own direction, so every output
   pixel reports the direction it sampled, and checks those against `march` at
   the shadow edge, the Einstein ring and far out: the shader is held to the
   physics before it is flown. Mass from the journal's StellarMass once
   journal_watch reads payloads (it reads event names only today), position
   from the draw. Behind `fix.black_holes`.
4. **Disks.** Journal Scan payloads for the feeding rule (companions' mass,
   radius, orbit), a deterministic orientation per hole, brightness calibrated
   against a star's surface in flight.
5. **Stars as points,** only if flight 1 shows B2: lens the starfield's
   vertices to their primary and secondary images instead of magnifying texels.

## What right looks like

A hole of mass M (units G = c = 1; M = 1.4766 km per solar mass) has its
horizon at r = 2M and the photon sphere, where light can circle, at r = 3M.
Light passing with impact parameter b < 3√3 M ≈ 5.196 M falls in. What a static
eye at distance r sees, all four of which Elite lacks or gets wrong:

- **The shadow.** A disk of true black, angular radius α with sin α = 3√3 M / r
  · √(1 − 2M/r) (Synge 1966); about 5.196 M / r when far. It is not the
  horizon's silhouette: it is 2.6 times the horizon radius, because light
  grazing the photon sphere is lost too.
- **The photon ring.** At the shadow's edge, a thin ring made of light that
  circled the hole once, twice, many times: every higher-order image of the sky
  (or of the disk) piles up there.
- **The Einstein ring and the secondary image.** A star exactly behind the hole
  becomes a ring; stars near that line become tangential arcs. Between the
  Einstein ring and the shadow the whole sky appears again, reversed and
  compressed, including the sky behind the viewer. Lensing is not local: close
  in, it bends every direction on the sky by more than a VR pixel.
- **The disk (feeding holes only).** A thin disk from the innermost stable
  orbit, r = 6M, outward, brightest at 9.55M (Page & Thorne 1974). Its far side
  is lensed up over the shadow and its underside shows below it. The side
  orbiting toward the eye is Doppler-boosted, bluer and much brighter; the
  receding side is redder and dimmer. The observed light of a blackbody at T is
  exactly a blackbody at g·T, g the redshift factor, so colour and brightness
  both follow from g with nothing to tune but the disk's own temperature.

Sizes for a 10 solar-mass hole (`python tools\blackhole_optics.py --at 10 KM`):

    distance      shadow     Einstein ring   light bent > 1 arcmin within
    600 km        14.33 deg   40.06 deg       179 deg of the hole
    1,500 km       5.81 deg   24.38 deg       178 deg of the hole
    3,000 km       2.92 deg   16.90 deg       177 deg
    30,000 km      0.29 deg    5.17 deg       147 deg
    1 ls           0.029 deg   1.62 deg        37 deg

A VR pixel is about 0.05 deg, so at supercruise distances of a light-second or
more a stellar-mass hole's shadow is sub-pixel and what shows is the Einstein
ring and the arcs; within a few thousand km the shadow dominates the view.
Supercruise reportedly lets a ship within about 25 to 67 km of a hole (whether
the HUD measures to the centre or to the horizon is unknown: flight 1 notes HUD
distances and phase 2 calibrates), which for the flight-1 target means:

    HIP 63835 B, 15.50 solar masses (M = 22.88 km, horizon 45.8 km)
    distance      shadow      Einstein ring
    100 km        122.2 deg   153.5 deg      (r = 4.4 M: the hole is most of the view)
    300 km         42.8 deg    76.3 deg
    1,000 km       13.3 deg    38.5 deg
    10,000 km       1.36 deg   11.3 deg
    53 ls (arrival) 0.0009 deg  0.28 deg

A supermassive hole scales with M. Elite's Sagittarius A* is 516,608 solar
masses per EDSM (M = 763,000 km); its Radius there, 15.55 solar radii, is not
its Schwarzschild radius (2.19), so r_s is derived from StellarMass, never read
from Radius. The two stellar holes checked agree with 2M as far as EDSM's
rounding shows.

Previews of the shader's own algorithm over a procedural sky, 10 solar masses
at 1,500 km, 50 deg field (not committed; regenerate):

    python tools\blackhole_optics.py --render bh-inactive.png --distance 1500 --fov 50
    python tools\blackhole_optics.py --render bh-feeding.png --distance 1500 --fov 50 --disk on --disk-gain 0.55

## What Elite does now

Nothing in EDVR has looked at the black-hole draw yet, and no mod or modder has
published its shader, hash or inputs (EDHM, DarkStarSword's 3d-fixes, the Helix
3D Vision fix: searched 2026-09-30, nothing). Community evidence, from search
summaries of forum threads (the forum was not directly readable; treat as
leads, not measurements):

- Only the background is lensed: skybox stars, nebulae, the Milky Way. Planets,
  companion stars and ships are never bent, including bodies behind the hole.
  Frontier patch 1.1 "forces background stars to render when the visible
  distortion is very high". A Frontier programmer's reply speaks of the effect
  as projected onto the skybox.
- The backdrop is a per-system six-face cube generated on jump, at 1024 to 4096
  per face by the galaxy-background or Environment quality (Odyssey default up
  to 2560, overridable far higher); its star patches are known to show as
  square blocks. The 30 June 2026 update reworked the galactic background, so
  older descriptions may be stale.
- In VR (2017) the swirl changed with head movement alone, "like a sticker on
  your helmet visor"; in 2015 the lens followed the pilot's head and Frontier
  called it a bug. Whether that is still so, and whether the eyes disagree, is
  unmeasured.
- No accretion disk, no jets, no modelled horizon; reports conflict on whether
  a dark centre is drawn at all.

## Hypotheses, and what tells them apart

How the lens is drawn (flight 1 settles it; none is assumed):

- **M1, the sky cube re-sampled.** A draw over the hole's region samples the
  galaxy cube with bent directions, before bodies draw. Signature: the pixel
  probe at the hole names a draw whose pixel shader declares
  `dcl_resource_texturecube`; its census SRV is `tex NxN` with N the backdrop
  size; body draws follow it in the eye sequence.
- **M2, the finished background re-sampled in screen space.** A copy of the
  eye's HDR target (a DCC line at eye size, R11G11B10) read back through
  distorted coordinates. Signature: that copy just before; `texture2d` at eye
  size in the disassembly and the census.
- **M3, bent inside the sky draws themselves.** The galaxy dome and starfield
  shaders take the hole as a parameter. Signature: no census signature is added
  when the hole comes into view; the probe names only sky draws.
- **M4, built offscreen and composited.** Signature: DCO lines (with
  census_offscreen) writing a target smaller than the eye, then a composite.

Why it is blurry (several may hold):

- **B1, source resolution.** Any raster source magnified up to ten times near
  the Einstein ring goes soft; signature: the SRV size above, and blur that
  changes with Elite's galaxy-background quality.
- **B2, stars as texels.** Background stars living in the cube as small patches
  become blobs when magnified; signature: square or smeared patches in the eye
  crops where arcs should be.
- **B3, a head-coupled lens (VR).** Signature: in a 16-frame eye run with the
  ship still and the head turning, the lensed pattern slides against the
  unlensed sky; or the two eyes' lens centres disagree.
- **B4, EDVR's own temporal pass.** TAA or DLSS runs on the tonemapped eye with
  motion that knows nothing of the lens; signature: sharper with Anti-aliasing
  off while the head moves, no change when still.

A physical replacement answers B3 by construction (it is computed per eye in
world space) and makes B4 checkable (the lensed sky is a known function of the
camera, so its motion can be written if TAA needs it). B1 and B2 decide the sky
source: whether the cube is good enough under magnification, or stars must be
lensed as points (moved in the starfield's vertex shader to their primary and
secondary image positions, sharp at any magnification).

## Flight 1: identification

Nothing to build: current HEAD, installed with `tools\install_edvr.py`. First
question afterwards, as always: `python tools\edvr_log.py --target steam
--expect-build HEAD`.

Settings (edvr.ini; set before launch, the first two need a restart):

    [log]      max_mb = 64                the log STOPS at 16 MB by default (4 MB
                                          before 2026-10-01), and one offscreen
                                          census is about 4 MB
    [advanced] glare_shader_dump = 1      every VS/PS/CS to edvr_logs\shaders
               census_offscreen = 1       a lens built offscreen shows as DCO
               census_lines = 16384       offscreen draws spend the cap fast
               pixel_probe = 0.5,0.5;0.46,0.5;0.54,0.5;0.5,0.44
    [hotkey]   dump_draws = NUMLOCK       dump_eyes = INSERT   (any free keys)

Target: HIP 63835 (275.5 ly from Sol; hole B about 53 ls from arrival, the
quickest in-system approach of the near ones; alternatives HIP 34707 B at 189
ly but 55,000 ls in, p Puppis B at 225 ly and 4,700 ls). Distances from
community listings: check the galaxy map before plotting.

1. **Control.** In the starting system, facing open sky: dump_draws.
2. **At the hole, far.** Target B so the HUD shows its distance. Face it,
   centred: dump_draws, then dump_eyes (the probe names every draw that changes
   the four points; the run saves 16 frames of eye crops). Note the HUD
   distance. Hold still for the eye run, then repeat dump_eyes turning the head
   slowly with the ship still (B3).
3. **Hole out of view.** Turn it behind you: dump_draws. Steps 2 and 3 in one
   system are the diff that names the lens draw.
4. **Closer, twice** (about 1,000 km and as close as supercruise allows): face
   it, dump_draws and dump_eyes at each; note distances.
5. **Constants (optional, one ini edit).** If a second screen is to hand:
   `python tools\diff_draw_census.py <gfx log>` prints the censuses it found,
   numbered; `--a <step 3's number> --b <step 2's number>` lists what the hole
   in view added. Set `census_cb_watch` to the added draw's vh (and try
   `census_cb_slot` 0, then 2), and dump_draws again at two distances. The
   census reads the watch when each census starts, and the ini reloads on save.
   This saves the calibration its own flight.
6. **B4.** Anti-aliasing off in the EDVR menu, dump_eyes while turning the
   head; back on, repeat.

Afterwards, on Windows: disassemble the named shaders with `python
tools\dxbc_disasm.py edvr_logs\shaders\ps_<HASH>.dxbc` (and its vs_) for the
lens's resource declarations and constant registers, and write the answers into
Status: which of M1 to M4, which of B1 to B4, the sky source's size and format,
and the draw's place in the eye sequence.

## How the shader will compute it

The ray from the eye lies in one plane through the hole. With u = 1/r and φ the
angle it sweeps, the orbit obeys u'' = 3Mu² − u, starting at u = 1/r with u' =
u √(1 − 2Mu) cot α, α the ray's angle from the hole's direction. March it in φ
with fixed-step RK4 (step π/96, cut on the way in so u never more than doubles
per step, and finished in closed form on the way out once less than a step from
infinity). u reaching 1/(2M) is capture: black. u returning to 0 after sweeping
φ_esc is escape, and the sky shows the direction cos φ_esc e1 + sin φ_esc e2
(e1 from the hole to the eye, e2 the ray's direction perpendicular to it). The
ray's plane meets the disk's plane at a fixed angle plus multiples of π, so
disk hits are exact interpolations of the orbit at those angles, and the
photon's angular momentum about the disk axis (constant, known at the eye)
gives g there.

Marching in φ rather than in space makes the cost independent of distance: a
ray from a light-second away and one from 600 km both take about 96 steps per
half-turn. The self-test holds the march to 1e-5 rad (a hundredth of a VR
pixel) against an independent quadrature, and both to the published shadow,
weak-field and strong-field results; see the tool's docstring for the list.

The GPU marches in single precision. A float32 mirror of the march (C++, the
same RK4, the Hermite root finished by three Newton steps) against the
quadrature, at eye distances 4M to 10⁶M: every ray sweeping less than 2π + 0.5
rad, which is everything out to the secondary image, is within 1.2e-5 rad. Only
rays sweeping more than 1.5 turns, a band under 0.1% of the shadow radius wide
(sub-pixel at every distance in the table above), reach 1e-4 to 3e-3 rad, where
the unstable orbit amplifies any rounding. A typical ray takes 100 to 130
steps, the photon ring's rays up to about 380.

Where the cost lands (per-pixel march, or a per-frame table of sweep against α
built by one small compute dispatch, which both eyes share) is decided with GPU
timings once the draw is known, not now.

## Choices that are Sean's

Recommendations first; none of these blocks the identification flight.

1. **Which holes feed.** An isolated stellar-mass hole accretes almost nothing
   and shows only its shadow and lensing: "nearly invisible" is the physically
   right look for most of them. A hole feeds when a companion star spills onto
   it: Roche-lobe overflow (the companion fills R_L/a = 0.49 q^(2/3) / (0.6
   q^(2/3) + ln(1 + q^(1/3))), q = M_star / M_hole, Eggleton 1983) or the
   captured wind of a close massive star, as in the real Cygnus X-1. The
   journal's Scan events carry mass, radius, semi-major axis and parents per
   body, so EDVR can decide per hole, deterministically, and disks stay rare
   and special. Recommended: that rule, with **all** as an option for anyone
   who wants spectacle over physics. The flight-1 target may be one: EDSM lists
   HIP 63835 B with a 1.3-day period, a close binary if confirmed. Sagittarius
   A* is the real exception worth copying: famously underfed, seen by the Event
   Horizon Telescope as a ring of hot plasma, not a thin disk; first version
   gives it shadow and lensing only.
2. **How a disk looks.** The Doppler boost and the gravitational redshift are
   not a style: they follow from the orbit and stay. The one free dial is the
   disk's peak temperature. A real luminous disk round a stellar hole peaks
   near 10⁷ K, which in visible light is a uniform blue-white whose two sides
   differ only in brightness; a cooler disk (the previews use 9,000 K) shows
   the full colour sweep, white-blue to orange to a dim red receding side.
   Recommended: 9,000 K default. Brightness against Elite's exposure is
   calibrated in flight against a star's surface, not chosen.
3. **Keys.** `fix.black_holes = on | off` (on: the physical shadow and
   lensing replace Elite's blob) and `fix.black_hole_disks = feeding | all |
   off`. Values name what the player gets, per AGENTS.md.

## Journal

### 2026-09-30: opened

No black-hole work existed in the repo (grep for black hole, lensing,
accretion, Schwarzschild: nothing). Added `tools\blackhole_optics.py` with its
self-test, gated in `build.bat` beside `check_status_blocks.py`.

What EDVR already has for flight 1, and what it lacks (read from source this
session, so the flight plan asks only for what exists):

- The census records per draw the VS and PS hashes, PS SRVs 0 to 7 and VS SRVs
  32 to 39 as `tex WxH fmt`, RTV0, DSV, viewport, blend and depth state
  (`draw_census.cpp`). It does not record an SRV's dimension, array size or
  cube flag (`describeResource` reads the Texture2D desc only), so a cube
  prints as its face size. The disassembly's `dcl_resource_*` lines answer that
  instead. `diff_draw_census.py` keys on vh, not ph.
- `pixel_probe` with an eye run names every draw that changes four chosen
  points, with both hashes. `glare_shader_dump` writes every shader created
  while it is on. `census_cb_watch` dumps a watched VS hash's constants (b0 or
  `census_cb_slot`, both stages) and is re-read at each census start, so it can
  be set mid-flight.
- `eye_draw_snapshot.h`'s capture takes Texture2D SRVs only and a hard-coded
  hash list, so it cannot copy the cube yet; not needed for flight 1.
- `journal_watch.cpp` keeps event names, no payloads: StarType, StellarMass and
  Scan are not read. Phase 3 needs StellarMass and phase 4 the Scan orbits; its
  chunked reader needs line buffering first (`carry[32]`).
- Shaders are HLSL in `R"HLSL(...)"` headers compiled at build time by
  `tools\temporal_shader_build` (a Variant and a LegacyContract row each). A
  pixel-shader swap has a template in `night_vision.cpp`; an extra pass of
  EDVR's own in `weapon_motion.cpp`; both have WARP rigs.
- The tonemap is a fullscreen draw from the R11G11B10 HDR target into the RGBA8
  eye, and EDVR's temporal pass runs on that RGBA8 eye, so the lens (in HDR,
  before tonemap) is upstream of TAA and DLSS: hence B4.

Community research (search summaries only; forums.frontier.co.uk and most fan
sites were unreachable from the session): see "What Elite does now". The
nearest-hole list and HIP 63835 B's 1.3-day period and 15.5 solar masses came
from EDSM and edastro listings via search; verify before flying. Two self-test
tolerances first failed against my own recollection of the series, not against
physics: the weak-field check needed the fifth- and sixth-order terms (3584/5
b⁻⁵ and 255255π/256 b⁻⁶), both confirmed by fitting the quadrature's residual
at b = 30 to 200, where the next coefficient converges to about 1.4e4 as it
should. The march's first form overflowed on a ray looking almost straight away
from the hole (u' ~ cot α is enormous there); fixed in the algorithm, not the
test, since the shader would have inherited it, and pinned by a regression
check.
