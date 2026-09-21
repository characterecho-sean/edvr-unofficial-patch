# The missing terrain at the edges of view

## Status

- **State (2026-09-20):** binary-analysis arc against build 332753 (the exe
  in `analysis\`, SHA-256 e6be8bbe…) has located the projection chokepoint;
  the flown separability probe (canted-projection.md, 2026-09-09) then
  reshaped the hypothesis. Goal: a memory patch replacing the
  render-wider-and-crop cull guard. Patch designed, not yet built; see the
  dated entry at the bottom.
- **The asymmetry-loss point:** the game wraps IVRSystem in a class
  (vtable VA 0x144E245B8; instance heap-held). Every VR projection query
  flows through exactly two wrapper methods:
  - `FUN_1404e2f50` (RVA **0x4E2F50**, slot 25): calls `GetProjectionRaw`
    and returns `{aspect, atan(|r|)+atan(|b|), atan(|l|)+atan(|t|)}` —
    tangent magnitudes folded into symmetric angle sums; the per-side
    asymmetry is discarded here (asm: analysis\decomp\cull_projection9.txt).
  - `FUN_1404e2d30` (RVA **0x4E2D30**, slot 27): calls
    `GetProjectionMatrix` and returns the matrix with row 3 negated —
    asymmetry (m02/m12) preserved. This is the renderer's path (the game
    extracts the four tangent elements and builds its own projection —
    canted-projection.md, the fold experiment).
- **Probe-constrained model (H3):** the culler follows neither channel
  live (`raw` lie inert, `matrix` lie inert for quads) and moved only in
  `both`, which also rebuilt the eye targets. Working model: the cull
  frustum is a **cached symmetric frustum computed at eye-target build**
  from one of these channels, combined per frame with the live view
  matrix. Which channel the cache derives from is unflown: the missing
  probe cell is one channel lied-to plus a forced target rebuild.
  Live-consumption variants (H1: culler reads the fov getter per frame;
  H2: reads and symmetrizes the matrix per frame) are refuted by the
  probe.
- **Ruled out (pointers, do not re-propose):** the
  `AstroSurfaceRenderManager::Cull` chain (`FUN_1412772b0` ->
  `FUN_143d097b0` -> `FUN_14444b4a0` -> `FUN_1444d0200`) is a **mono
  horizon-cone LOD culler**, not the view-frustum culler: its 48-plane
  table is built once at construction from planet geometry
  (`FUN_144497d30`), its fov scalar feeds only the LOD screen-size gate
  (`tan(fov/2)` at subobj+0x8E0, written by `FUN_14448e0b0`), and the
  per-object worker `FUN_14444d6c0` is LOD-band selection, not a frustum
  window test. Decompiles in `analysis\decomp\cull_round3*.txt`. Also
  ruled out: `EnableFrustum0Override` / `CullingBias` are shadow-cascade
  config (`FUN_1428555A0`), unrelated to terrain tile culling.
- **Established:** the game loads `openvr\win64\openvr_api.dll`
  dynamically (RVA 0x4E4870), holds `IVRSystem_012` in global
  VA 0x145F1A860, and reaches it only via the wrapper. LibOVR impl
  vtable at VA 0x144E243F0 with parallel slots (slot 25 -> RVA 0x4E2F30).
- **Next:** fly the channel probe, which now exists in the native runtime
  (2026-09-20) as `advanced.cull_guard_channel`: `raw` one session,
  `matrix` another, guard on, watch the edges over terrain. Covered under
  `raw` = the cache derives from the fov getter; covered only under
  `matrix` = from the matrix getter. That answer gates the memory patch:
  hook the fov getter (`0x4E2F50`) in observe+widen modes,
  installed at startup so the game's initial eye-target build sees widened
  values; guard OFF; fly. Tiles clean from session start = the cache
  derives from this getter and the patch stands. Tiles still dropping =
  the cache derives from the matrix channel; the getter census plus a
  matrix-getter census guides the next static round. Note the implication
  of H3 for tuning: mid-session margin changes are not live for the
  culler — they take effect at the next target rebuild (quality toggle),
  not on ini save.

*Frontier issue [72609](https://issues.frontierstore.net/issue-detail/72609) —
"Culling of planet surface in VR too aggressive", a recurrence of
[37119](https://issues.frontierstore.net/issue-detail/37119), which was
fixed once and marked so.*

This is a write-up of what the bug actually does, measured rather than
guessed, and what EDVR does about it from outside the game. It is written for
whoever might fix it properly, inside the game, where it can be fixed at zero
rendering cost.

**Short version: Elite culls planet terrain against a frustum narrower than
the one it renders, in the way a culler behaves when it treats the per-eye
frustum as symmetric.** VR eye frusta are strongly asymmetric, so terrain
tiles inside the visible outer edges are never drawn, and the player sees
black squares where ground should be. Report a *symmetrized* frustum to the
game — while showing the player exactly what was shown before — and the
missing tiles come back. Report the truth again and they vanish again. The
culler follows the report, not the optics.

---

## The symptom

Over a planet surface — gliding down, flying low, or just turning your head
at altitude — squares of terrain at the edges of view are missing, showing
black, and pop in and out as the view moves. The tracker has confirmations
across Quest 3, Valve Index, Bigscreen Beyond and Pimax Crystal, at every
graphics setting ("even on the lowest"), and the original report notes the
same tiles vanishing slightly too early at the edges of a flat monitor.

Two things make it a VR complaint in practice. A monitor's frustum is
symmetric, so a symmetric-assumption culler is only wrong there by whatever
thin margin the tuning left; a headset's per-eye frustum is not symmetric at
all, and the error grows to several degrees. And the edge of a monitor is
peripheral by definition, while the edge of a headset's view is somewhere
your eyes actually go — especially through pancake lenses, which are sharp
to the very edge. That is why Quest 3 reports dominate the tracker: not the
worst-affected headset, the best-corrected one.

## What is actually happening

Everything below was measured through an `openvr_api.dll` proxy that records
what the game asks the VR runtime, and can answer differently. No game file,
game memory or game code is touched at any point.

**Elite queries the projection continuously.** `GetProjectionRaw`,
`GetProjectionMatrix` (near=1, far=50,000, DirectX convention),
`GetEyeToHeadTransform` and `GetRecommendedRenderTargetSize` are each called
about a dozen times per frame, every frame — roughly 1,080 calls per second
each at 90 Hz. Whatever consumes them re-reads them live.

**The per-eye frustum is strongly asymmetric.** On the Quest 3 rig that
reproduces the bug (via Virtual Desktop), the left eye's tangents are:

| Edge | Tangent | Half-angle |
|---|---|---|
| outer (l) | −1.3764 | 54.0° |
| inner (r) | +0.8391 | 40.0° |
| t | −1.4281 | 55.0° |
| b | +0.9657 | 44.0° |

94.0° × 99.0° per eye, mirrored for the right eye, with the wide side
outward — the normal shape of VR optics.

**A centered frustum of the same extent misses the visible outer edges.** A
culler that keeps the frustum's half-angles but centres them covers about
±47° horizontally where the eye actually sees 54° outward — roughly **7° of
visible-but-culled terrain at each outer edge**, which is precisely where the
tracker's reports put the missing squares.

**The decisive experiment.** Report each axis widened to ± its larger
tangent — the left eye becomes ±54.0° × ±55.0°, a symmetric superset of the
truth — while (a) asking the game for correspondingly larger render targets
so pixel density is unchanged, and (b) handing the runtime only the
true-frustum region of each rendered frame, at the same texture size the
session had always submitted. Nothing the player sees changes except one
thing: **the missing tiles are gone, at every edge**
(`edvr_vr_20260818_125338.log`, build v0.7.5-10-g99aca9c). Turn the guard
off and they return.

That establishes, from outside the binary:

1. The terrain culler's frustum is **derived from the projection the game
   queries**, not from a hardcoded angle — lie to the query and the culler
   follows, live, mid-session.
2. A **symmetrized** frustum is sufficient to cover it on a strongly
   asymmetric eye. This is consistent with — though not strict proof of —
   the culler assuming a centred frustum; the minimal sufficient margin has
   not been walked down yet.

## What it is not

- **Not terrain streaming or generation lag.** The tiles pop in *and out*
  with head rotation. Generation lag fills in once and stays; nothing about
  rotating your head un-generates a tile.
- **Not a settings problem.** Reported identical at minimum settings, and
  the terrain work/quality sliders move tile *detail*, not tile
  *visibility at the edge*.
- **Not headset-specific.** The same class shows on a flat monitor, thinner.
  Which headsets *notice* it is mostly an optics question — edge-sharp
  lenses and no peripheral foveation make it unmissable.

## What a fix inside the game would look like

The renderer and the culler must use the same frustum. The game already
queries the true asymmetric per-eye tangents a dozen times a frame;
wherever terrain tiles are tested for visibility, that test appears to use
a symmetric approximation of them instead. Culling against the union of the
two eyes' true frusta — plus whatever small guard band covers one frame of
head motion at the pose-prediction horizon — removes the artifact at zero
rendering cost, and the same change covers the flat-screen version of the
complaint, whose margin is evidently also thin.

Worth knowing: this was fixed once. Issue 37119 — "culling is too
aggressive, hiding stuff that is visible on the edges of the view" — was
closed as fixed in the Odyssey era, and the class returned in Trailblazers
(4.1). Wherever the frustum choice lives, it is somewhere a rebuild can
quietly regress.

## Reproducing the diagnosis

The artifact itself needs only a planet and a headset. The *diagnosis* — 
that the culler follows the reported projection — needs the one experiment
above: change what `GetProjectionRaw`/`GetProjectionMatrix` return, keep
what the player sees constant, and watch the tile set follow the report.
EDVR's `observe_projection` mode (on by default) logs the tangents any rig
actually reports, which is also how the numbers in this document were
collected on two headsets in one afternoon.

---

## What EDVR does about it — the cull guard

`cull_guard = symmetric` under `[fix]` in `edvr.ini`, **off by default**,
and it must be set before launch (turning it *off*, or changing mode or
margin, is live; turning it *on* installs a hook that only installs at
startup).

The guard tells the game a wider frustum than the headset shows, in one of
two modes — `symmetric` (each axis to ± its larger tangent: the exact fit if
the culler centres the frustum) or `percent` (every tangent widened by
`cull_guard_percent`: a plain margin, for finding the smallest number that
keeps the edges clean). It then:

1. **Asks the game for larger render targets** in the same proportion, so
   the wider frustum is rendered in *new* pixels and nothing gets softer.
   Elite adopts the changed recommendation live — measured at about 14
   seconds to rebuild its eye targets mid-session, no restart.
2. **Starts the projection lie only after both eyes submit at the new
   size**, at a frame boundary, so every answer within a frame — raw
   tangents, the projection matrix, and the submitted image at the end —
   tells one story. "New" is measured against the sizes the game was
   submitting when stage 1 began, seeded from the first submissions after
   that boundary (2026-09-02: with Elite's HMD Quality at 1.25 the
   un-rebuilt 1.25x targets already cleared the size threshold, and the
   guard adopted a canonical the game abandoned two seconds later).
3. **Hands the runtime only the true-frustum region** of each frame, copied
   into an EDVR-owned texture at exactly the size the session had always
   submitted.

The cost is GPU load, not sharpness: full symmetric mode on the Quest 3
frustum above renders about 48% more pixels. Two knobs cut it.
`cull_guard_fraction_h` / `_v` cover only part of each axis's shortfall —
live-tunable, so the staircase described in `edvr.ini` finds the cheapest
value that still keeps the edges clean, and the log's `cull guard margins`
line names what each step leaves uncovered. On the Quest 3 frustum the
staircase settled at **`fraction_h = 0.25`, `fraction_v = 0` — about 6%
extra pixels** — with the edges still clean: the vertical margin proved
entirely unnecessary (matching the tracker's sides-only reports), and the
outermost ~4.4° horizontal either genuinely needs no cover or is not
resolvable through the lens edge. And `cull_guard_headsets` gates the whole
guard to listed FOV signatures — the log prints each headset's, like
`94x99` — so a rig that swaps headsets pays only on the headset its owner
listed, with no config edits at swap time; unlisted headsets run
observation only, at no cost.

One suspected side effect, measured and cleared the same day it was
instrumented — recorded here because the first reading blamed the guard.
An early tuning flight showed the transition-flash fix's recognition
counters climbing with the margin (29 at guard-off against 3,277 at
`fraction_h = 0.5`, with withheld frames felt as judder), and the obvious
reading was that the wider frustum admits more near-surface render passes
for the detector to learn. So the detector was taught to attribute before
anything was changed: it stamps every camera-history frame with the
margin that was live when it was drawn, splits its running totals under
the guard, and prints its learned tables when the history is dumped. The
staircase then re-flew with per-frame attribution, including a guard-OFF
control window mid-flight, and the correlation inverted: the guard-off
stretches carried the *highest* recognition rates of the day, and the
camera populations doing the churning appear identically with the guard
off. The churn belongs to low flight over terrain, not to the margin —
the earlier numbers had confounded margin with flight profile. No
margin-aware detection is needed: the flash fix certifies the recurring
geometry within a few frames and excuses it from then on, its burst
governor bounds any storm, and a withheld frame is resubmitted on time
rather than stalled. The attribution stays in the build, so any future
"is this the guard?" report answers itself from one history dump.

### Why the submit side is this elaborate

Because the two obvious designs are both refuted, in the field, on the same
day — recorded here so neither is rediscovered:

- **Narrowing the submitted texture bounds** (`VRTextureBounds_t`) is
  correct by the OpenVR contract and free. OpenComposite over VDXR ignored
  the narrowed bounds and displayed the full wide-rendered image against the
  true-FOV mapping — experienced as the whole world distorting with every
  head turn. It survives as `advanced.cull_guard_submit = bounds` for
  measuring whether real SteamVR honours it; `copy` is the default.
- **Submitting the cropped region at its natural size** was clean by every
  contract read — and sheared the image into a parallelogram under head
  rotation, because the session's first-ever submission was then a texture
  whose aspect the transport had never served. The lesson, twice paid:
  **that transport stack reliably serves only submission shapes the session
  has already established.** Hence the two-stage go-live and the crop
  snapped to the canonical size — the runtime never sees a shape change at
  all.

One more recorded assumption that cost a flight: texture v runs from the
**positive** vertical tangent (v=0 is the *b* edge — derived from the
runtime's own matrix, `(1+m12)/m11 = b` at NDC y=+1, D3D11's top row).
The first build measured the vertical crop from *t*, kept the wrong end of
every column, and turned forward leans into a vertical stretch. The test
fixture's vertical is asymmetric now, so a flipped axis cannot pass a build.

### Guard rails

The guard edits `GetProjectionMatrix` only after checking, per eye, that
the runtime's matrix actually matches the tangent formula the edit assumes;
a runtime that builds its matrix differently makes the whole guard **inert,
loudly**, and everything forwards the truth. Any submit-side failure stands
the guard down at the next frame boundary — one plainly-shown wide frame at
worst, never a mismatched crop. Foreign consumers of the hooked interface
always receive the truth; the lie is for the game alone.

### Reading the log

A working session says, in order: `cull guard stage 1` (targets asked
bigger), then two `cull guard LIVE` lines naming the true and reported
tangents, the crop, and `submissions stay at the session's own size`. The
exit totals count crops by mechanism. `cull guard INERT` or
`STANDING DOWN` means the guard refused to run on this rig's runtime —
everything still renders normally, and the log's own line says exactly why.

**If terrain squares are still visible with the guard live:** switch
`cull_guard` to `percent` and raise `cull_guard_percent` (both live — save
the file, no restart). If a 20% margin does not move the artifact at all,
what you are seeing is probably not this bug — send `edvr_logs\` either
way, because the LIVE lines plus the tangent lines are what distinguish
"guard not engaged", "margin too small", and "different bug entirely".

### Known limits, stated plainly

The guard has been field-verified on both of this project's rigs, which
are both OpenComposite paths: Quest 3 over Virtual Desktop (where the
missing tiles reproduced, and are gone) and Pimax over PiOpenXR (where the
guard runs clean and invisible — horizontal-only there, about 19% extra
pixels, since that frustum's vertical is already symmetric; that rig never
showed the tiles, so it verifies the machinery rather than the cure). Real
SteamVR has not been measured at all — the observation half ran there long
before the guard existed, but the guard itself has not. And the game's
culler is being *covered*, not fixed: the tiles were always renderable,
and the correct fix is one line of frustum arithmetic away from whoever
owns the culler.

---

## 2026-09-20 — the projection chokepoint, and a proposed memory patch

Static analysis against build 332753 (Ghidra project `analysis\ghidra`,
scripts `analysis\ghidra_scripts\CullProjection*.java`, decompiles
`analysis\decomp\cull_projection*.txt`) followed the projection query from
the OpenVR import to the point where per-eye asymmetry is discarded.

**The chokepoint.** The game loads `openvr\win64\openvr_api.dll`
dynamically, stores `IVRSystem_012` in a global (VA 0x145F1A860), and wraps
it in a class (concrete vtable VA 0x144E245B8, heap instance). Every
projection query in the game flows through two wrapper methods — they are
the only call sites of `GetProjectionRaw` (vtable +0x10) and
`GetProjectionMatrix` (+0x8) in the binary:

- **Fov getter** `FUN_1404e2f50` (RVA 0x4E2F50, wrapper slot 25). Per eye
  it queries the raw tangents and the render-target size, then returns
  three floats: `aspect = resX/resY`, `atan(|right|)+atan(|bottom|)`,
  `atan(|left|)+atan(|top|)` (helper at RVA 0x48B54DC is the CRT `atanf`;
  abs masks via `ANDPS 0x7fffffff`; full asm verified in
  cull_projection9.txt). Angle sums of magnitudes — **the per-side
  asymmetry dies here**. Whatever consumes this triple can only build a
  symmetric frustum.
- **Matrix getter** `FUN_1404e2d30` (RVA 0x4E2D30, wrapper slot 27).
  Returns the projection matrix with row 3 negated; m02/m12 offsets
  intact. The renderer's path — asymmetry survives.

This bifurcation is the whole bug shape: the renderer draws the true
asymmetric frustum, while any culler fed by the fov getter culls a
centered one of the same total extent — on the Quest 3 rig, ±47° against
an eye that sees 54° outward. It also explains why the cull guard works:
it lies at the OpenVR query, upstream of both paths, and pays for the lie
with extra rendered pixels because the *renderer* also believes it.

**What the flown probes add (canted-projection.md, 2026-09-09).** The
separability probe lied through one projection call at a time: `raw`
alone did not move the terrain quads, `matrix` alone widened the picture
but not the quads, and only `both` — which additionally enlarged the
render targets and so forced an eye-target rebuild — covered them. Read
against the chokepoint above, this says the terrain culler does **not**
consume either getter live, per frame. The working model is a **cached
cull frustum**: computed once per eye-target build from one of the two
channels (which one is unflown — the probe's missing cell is one channel
lied-to plus a forced rebuild), symmetric by construction if it derives
from the fov getter's angle sums, and combined per frame with the live
view matrix — which is why tiles still pop with head rotation under a
cached frustum shape. It also explains the guard's own mechanics: the
guard only moves the quads because its stage 1 forces the rebuild that
recomputes the cache.

**Proposed patch: replace the fov getter, leave the matrix getter alone.**
Inline-hook `FUN_1404e2f50` via the existing `code_hook` relay machinery
(same pattern as `kinematic_eval_hook.cpp`: RVA + prologue-bytes check +
PE timestamp gate, inert on any other build). The hook reimplements the
getter — it is 60 instructions — calling the wrapper's own IVRSystem
pointer (`this+8`) for tangents and render size, then computes the
outputs from tangents widened to the per-axis symmetric superset,
`m_h = max(|l|,|r|)`, `m_v = max(|t|,|b|)`: both angle sums become
`atan(m_h)+atan(m_v)`. Cullers fed by this getter then cover the true
per-eye frusta; the renderer, on the matrix getter, never sees the
change. Cost: zero extra pixels, zero crop machinery — the cull guard's
entire stage 1-3 apparatus becomes unnecessary for rigs where this holds.
Under H3 the hook must install **at startup**, so the game's initial
eye-target build already sees widened values, and mid-session margin
changes take effect only at the next target rebuild (a quality toggle),
not on ini save.

Modes (config names functionality): `off` (default until flown),
`observe` (true values out, but log a census of call-site return addresses
and per-eye values — names every consumer of the triple in one session),
`widen` (symmetric-superset values out, plus the census). Mutually
exclusive with `fix.cull_guard` at startup, loudly.

**Verification, one flight:** guard OFF, hook in `widen` from launch, near
a planet surface. Tiles clean from session start = the cache derives from
the fov getter and the patch stands (H1-as-cached confirmed). Tiles still
dropping after a forced target rebuild (toggle HMD quality to force it)
= the cache derives from the matrix channel (H2-as-cached), and the
census on both getters — call sites, rates, values — names the consumers
for the next static round. **Watch item:** the AstroSurface LOD gate's
scalar fov (renderer field +0x2C, read in `FUN_141277370`) may be fed
from this getter; widening shifts terrain subdivision thresholds
slightly. Compare LOD/pop behaviour and the `cull guard margins` numbers
against a guard-off baseline in the same flight.

**Why not patch the binary on disk:** EDVR's whole value is per-build
gating and inert-by-default failure; a runtime hook with a prologue check
keeps that. The RVA is build-specific (332753); other builds get the
guard, as today.

**The probe's missing cell is now flyable.** The legacy proxy's
`advanced.cull_guard_channel` (976b4f2, removed with the proxy in 1a54e9e)
is reimplemented in the native OpenXR runtime, with one deliberate
difference from the flown probe: the split channel no longer shrinks the
pipeline. The grown size ask, the adoption and the crop run unchanged, so
the eye-target rebuild that recomputes the cull cache still happens; only
the lie is split. `both` (the default) is the guard exactly as flown;
`raw` tells the widened frustum to GetProjectionRaw alone (the matrix
channel answers the content frustum, and the crop is the identity);
`matrix` tells it to GetProjectionMatrix alone (the raw channel answers
the content frustum). Flight protocol: guard on, `channel = raw` for one
session, `= matrix` for another, parked over terrain, watching the edges.
Tiles covered under `raw` means the cache derives from the fov getter;
covered only under `matrix` means it derives from the matrix getter — and
the patch above hooks the wrong place.
