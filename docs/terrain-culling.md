# The missing terrain at the edges of view

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
   tells one story.
3. **Hands the runtime only the true-frustum region** of each frame, copied
   into an EDVR-owned texture at exactly the size the session had always
   submitted.

The cost is GPU load, not sharpness: full symmetric mode on the Quest 3
frustum above renders about 48% more pixels. Two knobs cut it.
`cull_guard_fraction_h` / `_v` cover only part of each axis's shortfall —
live-tunable, so the staircase described in `edvr.ini` finds the cheapest
value that still keeps the edges clean, and the log's `cull guard margins`
line names what each step leaves uncovered (halving both fractions costs
about 23% instead). And `cull_guard_headsets` gates the whole guard to
listed FOV signatures — the log prints each headset's, like `94x99` — so a
rig that swaps headsets pays only on the headset its owner listed, with no
config edits at swap time; unlisted headsets run observation only, at no
cost.

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
