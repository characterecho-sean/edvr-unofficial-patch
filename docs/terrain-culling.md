# The missing terrain at the edges of view

## Status

- **State (2026-10-09, evening): REOPENED -- a residual on RAPID head turns is
  under investigation.** The head-pose fix ships as behaviour, with no key.
  Elite's game thread asks the runtime for the head pose "now"
  (GetDeviceToAbsoluteTrackingPose, return RVA 0x4E3881, prediction about 0 s)
  and culls planet terrain with it, while the frame is drawn with the pose at
  its display time. Flight 3 (Crystal Super, Pimax OpenXR): that pose was
  **41 to 44 ms** older than the drawn one and up to **4.1 deg** from it, r =
  **+0.98** against head speed. Answering it at the drawn frame's display time
  (`display`) stopped the squares on gaze switches. See "What EDVR does now".
- **Residual (rc flight, v0.18.3-164-gf9597bd3, Crystal Super, Pimax OpenXR).**
  Gaze switches are clean, but black terrain squares still show at the OUTER
  edges on RAPID head turns on approach to a planet. The runtime log says
  `display` acts fully there (gap 0 ms, mean angle to the latest frame's render
  pose 0.003-0.029 deg, fallbacks only while loading), yet one window reads a
  max of 2.8 deg. **H-onef (untested):** the game thread prepares frame N+1
  while the render thread draws N; during N's render the latch is set, so it
  reuses the pose cached for N and its cull camera is drawn one frame later.
  `display` then leaves one period (~11 ms; 2.8 deg at 250 deg/s).
- **Test build (NOT FLOWN, TEMPORARY).** `advanced.cull_pose` = `display`
  (default, = what ships) | `now` | `next` | `display_direct` | `next_direct`,
  live; the `_direct` modes hot-patch the latch branch (build 332841 only). The
  `pose gap:` line gains `mode` and `angle-to-next-drawn` (the pose each
  game-thread caller was handed in frame N against the render pose of N+1):
  about 0 under `next`, head speed x one period under `display`. Frame ABI:
  `EdvrNativeFrameOutput` version 6 adds one `cullPose` word after the fade
  level (an older half, or a code past 4, reads display); versions 7 and 8 of
  earlier test builds stay deleted. Protocol and verdict rule: "2026-10-09, rc
  flight" at the end.
- **What ships.** A call to GetDeviceToAbsoluteTrackingPose is answered at the
  latest frame's predictedDisplayTime when its return address is inside the
  game's own image and its prediction is under 5 ms either way; any other
  caller is located as it always was. No build gate. The first such call logs
  `head pose: Elite's "now" requests are answered at the drawn frame's display
  time (first from exe+0x...)`, up to 8 further callers likewise. The cached
  display time is dropped wherever the origin, session or geometry publication
  is invalidated, is not used once more than one period (at most 50 ms) behind
  now, and a TIME_INVALID is retried once at now + prediction.
- **Removed (2026-10-09, five commits):** the terrain guard with all six keys
  (`fix.cull_guard`, `_percent`, `_fraction_h`, `_fraction_v`, `_headsets`,
  `advanced.cull_guard_channel`; the installer carries an old line as "no
  longer used") and the instrument `advanced.cull_probe` (caller census,
  selective lies, mono camera hooks, `--tally cull`). `advanced.cull_pose` went
  with them and is back, in a new form, as the test key above. The FOV trim
  that shared the guard's stage machine is untouched (`native_fov_trim.h`).
- **Ruled out** (evidence under "Status detail"): a static frustum deficit (a
  steady head shows no squares), the fov getter as the culler's input, the
  09-23 "same +19.4% ask" premise, H3, the union model, H2, FUN_13ACC40 as the
  consumer. **UNRELIABLE:** the 09-23 verdict "the culler follows
  `GetProjectionRaw`, not the matrix" (a pilot's judgement under periphery
  flicker, a 10-degree trim and a 1.6% difference).
- **Next:** one flight of the test build, same approach and the same rapid
  turns, three runs: `display` (control), `next`, `next_direct`. Read
  `python tools\edvr_log.py --tally pose` for the angle-to-next-drawn of
  Elite's caller (exe+0x4E3881) per mode, and the squares by eye. The pilot's
  one earlier sighting under `next_direct` (one frame of over-lead) is a reason
  to read `next` before `next_direct`.

## The bug in short

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

## Status detail (moved out of Status 2026-09-29)

**The Status block as it stood at the flight-3 result, moved verbatim
2026-10-09** (it names the temporary instruments, `advanced.cull_probe` and
`advanced.cull_pose`, which no longer exist, and the old guard):

- **State (2026-10-09, flight 3): ROOT CAUSE CONFIRMED, FIX FOUND.** Elite's
  game thread asks for the head pose "now" (GetDeviceToAbsoluteTrackingPose,
  return RVA 0x4E3881, prediction ~0 s) and selects terrain with it; the frame is
  drawn with the display-time pose. Measured on the Crystal Super / Pimax
  OpenXR, build e256e9bb, `edvr_log.py --tally pose` on
  `edvr_openxr_20261009_144149_193_26440.log`: under `off` that pose is
  **-41 to -44 ms** from the drawn frame's display time and turned 0.9 deg mean,
  up to 4.1 deg, from the drawn pose, r = +0.98 against head speed. Pilot:
  `display`, `next` and `display_direct` all stopped the squares on gaze
  switches; one unreproduced possible sighting under `next_direct` (one frame
  of over-lead, 0.65 deg mean). **Fix: answer that call at the drawn frame's
  display time (`display`); no engine patch, no extra pixels, no tuning.** The
  latched-pose bypass is not needed. The old guard (render inflation + crop,
  ~6% GPU at h=0.25/v=0) was a margin covering this lag. Ruled out: a static
  frustum deficit (squares need head motion). Dated entries at the bottom.
- **Pilot report 2026-10-09 (Crystal Super, Pimax OpenXR):** the squares show
  only on descent into a landable planet, come and go with head movement as the
  gaze switches, and fill in quickly once the head is steady. **Ruled out on this
  rig:** a static frustum deficit (the centred-cull models, H-cam included),
  because a steady head shows no squares. The 447de48f steady-view protocol
  (`cull_probe = measure | cycle | mono`, `--tally cull`) cannot see a lag and
  was NOT flown; its counter, census and mono observers stay available.
- **Round 6, READ** (build 332841; `analysis\decomp\verify_20261009_cull6_*`).
  Elite's head-pose function 0x4E3690 has two sources. *Render thread* (arg6 =
  1): WaitGetPoses' render pose (call 0x4E3715), latched at [W+0x111] until
  Present clears it; EDVR locates it at the frame's predictedDisplayTime, so it
  is the drawn pose. *Game thread* (arg6 = 0, the controller tick, tid 24212):
  IVRSystem::GetDeviceToAbsoluteTrackingPose (`call rbx` at 0x4E387F, return RVA
  0x4E3881), prediction about 0 s, i.e. "now"; with the latch set it reuses the
  cached WaitGetPoses pose instead, by `je 0x4E384F` at 0x4E36EE (0F 84 5B 01 00
  00; reached with arg6 = 0 only). EDVR's runtime (openvr_system.cpp,
  native_runtime_host.h locateHead, head_locator.h) located that call at
  QPC-now + prediction.
- **Round 6, INFERRED (H-lag):** the planet-terrain culler takes the game-thread
  camera, so its tiles are chosen with a pose at least a frame older than the
  drawn one and a head turn's leading edge goes missing. (Census: eye cameras
  are built twice a frame, via wrapper slot 16 and a second thread via slot 26.)
- **Instruments on e256e9bb (temporary):** a `pose gap:` line per caller every
  2 s in the runtime log (`edvr_log.py --tally pose`), and `advanced.cull_pose`
  = off | display | next | display_direct | next_direct (`_direct` adds a 2-byte
  latch-bypass hot patch at 0x4E36EE). Build 332841 only.
- **Next:** ship `display` as the behaviour with no key, propose removing the
  old guard's keys and every temporary instrument (quoted, asked first), then
  fly once more on the cleaned build, ideally on the Quest 3 too.
- **Ruled out** (evidence under "Status detail"): the getter as the culler's
  input, the 09-23 "same +19.4% ask" premise, H3, the union model, H2, FUN_13ACC40
  as the consumer. **UNRELIABLE:** the 09-23 verdict "the culler follows
  `GetProjectionRaw`, not the matrix" (a pilot's judgement under periphery
  flicker, a 10-degree trim and a 1.6% difference).
- **Earlier instruments** (rounds 3-5): `advanced.cull_probe` = off | all | camera
  | ui | sky | sizes | other | mono | cycle | measure (lies need `fix.cull_guard`
  off and build 332841): the caller census, the selective lie, the terrain-draw
  counter, the mono camera hooks; their eliminations are in the rounds 3-5 entry.

**Earlier moves out of Status:**

**Ruled out** (moved verbatim from the Status block; do not re-propose):

- The cached-frustum model (H3: culler derives its frustum at eye-target
  build and keeps it until the next rebuild) — refuted 2026-09-23:
  switching to `matrix` (raw channel honest) mid-session brought the
  squares back with **no** target rebuild in between.
- The union model (culler keys on the wider of the two channels;
  widening either suffices) — refuted by the same observation.
- The matrix-channel model (H2) — refuted: `matrix` mode lied wide on
  the matrix and the squares showed regardless.
- The 2026-09-09 legacy-probe `raw`-inert reading — superseded; see the
  2026-09-23 entry (instrument artifact, marked inference).
- The `AstroSurfaceRenderManager::Cull` chain (`FUN_1412772b0` ->
  `FUN_143d097b0` -> `FUN_14444b4a0` -> `FUN_1444d0200`) is a **mono
  horizon-cone LOD culler**, not the view-frustum culler: its 48-plane
  table is built once at construction from planet geometry
  (`FUN_144497d30`), its fov scalar feeds only the LOD screen-size gate
  (`tan(fov/2)` at subobj+0x8E0, written by `FUN_14448e0b0`), and the
  per-object worker `FUN_14444d6c0` is LOD-band selection, not a frustum
  window test. Decompiles in `analysis\decomp\cull_round3*.txt`. Also
  ruled out: `EnableFrustum0Override` / `CullingBias` are shadow-cascade
  config (`FUN_1428555A0`), unrelated to terrain tile culling.
- The fov-getter patch (hook RVA 0x4E2F50, return angle sums widened to the
  per-axis superset, modes off/observe/widen) — WITHDRAWN 2026-10-09: the
  getter is not the culler's input.
- The getter as "the culler's fov source" and "the asymmetry-loss point" —
  refuted 2026-10-09: its outputs are read only by the eye camera setter and
  the UI scale, and its read outputs were identical in the 09-23 windows.
- The 09-23 "raw and matrix windows asked for the same +19.4%" premise —
  refuted 2026-10-09: from 05:16:29 a 10-degree outer trim was on, the ask was
  2554x3032 in both modes, and raw differed from matrix only on the inner
  edge, 1.6% of span.
- The getter's "r+b / l+t" output pairing — wrong: atan|t|+atan|b| vertical,
  atan|l|+atan|r| horizontal, aspect first (2026-10-09).
- FUN_13ACC40 as H-cam's consumer — refuted 2026-10-09 (round 4): it is
  "FSSRenderingComponent".
- The 09-23 verdict "the culler follows `GetProjectionRaw`, not the matrix" —
  demoted to UNRELIABLE 2026-10-09 (not ruled out): a pilot's judgement under
  periphery flicker, with a 10-degree outer trim on and a 1.6% difference.

**Established, moved from Status 2026-10-09:** the game wraps IVRSystem in a
class (vtable VA 0x144E245B8; instance heap-held), loads
`openvr\win64\openvr_api.dll` dynamically (RVA 0x4E4870) and holds
`IVRSystem_012` in global VA 0x145F1A860. The matrix wrapper is `FUN_1404e2d30`
(RVA 0x4E2D30, slot 27): it calls `GetProjectionMatrix` and returns the matrix
with row 3 negated, asymmetry (m02/m12) preserved; the renderer's path
(canted-projection.md, the fold experiment). The LibOVR implementation's
vtable is at VA 0x144E243F0 with parallel slots (slot 25 -> RVA 0x4E2F30).

**Corrections and established, moved from Status 2026-10-09 (round 5):**

- **Corrections** (Elite build 332841, FileVersion 332841 / ProductVersion
  4.4.1.1; the "332753" label was wrong): `FUN_1404e2f50` (RVA 0x4E2F50) returns
  `{aspect = recommended W/H (not tangents), atan|t|+atan|b| (vertical),
  atan|l|+atan|r| (horizontal)}`, not the old "r+b / l+t"; only out[0] and out[1]
  are read, by the eye camera setter 0x2878DC0 and the UI scale at 0x2842AE3
  (k = tan(0.782)/tan(vFOV/2)), so widening the getter resizes the UI.
- **Established** (`analysis\decomp\verify_20261009_cull2_*`): six
  `GetProjectionRaw` call sites in three wrappers (0x4E2FA5 in the getter;
  0x4E3C93 via 0x4E3C50, a quad pass, INFERRED sky; 0x4E42FA, 0x4E4351,
  0x4E43A6, 0x4E43F4 in 0x4E4270, UI sizes); the getter's aspect comes from its
  own GetRecommendedRenderTargetSize call, expected to return at 0x4E2FBE (the
  census confirms). Static analysis found no culler.

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

## What EDVR does now

The culler's input was a head pose Elite asks for on its game thread, 41 to 44
ms older than the pose the frame is drawn with. The runtime now answers that
one request at the drawn frame's display time. Nothing is widened, nothing is
cropped, no pixels are added, and there is no key.

**Measured (flight 3, 2026-10-09, Crystal Super via Pimax OpenXR, build
e256e9bb, `edvr_openxr_20261009_144149_193_26440.log`, `edvr_log.py --tally
pose`).** Under `off` the call returning to exe+0x4E3881 was located
**-41 to -44 ms** from the drawn frame's display time, its pose turned **0.9
deg mean and up to 4.1 deg** from the drawn pose, and that angle correlated
**r = +0.98** with head speed. Under `display` the gap is 0 by construction and
the angle 0; `display`, `next` and `display_direct` all stopped the squares on
gaze switches, and the shipped behaviour is `display`.

**Which calls.** The filter (src/openxr/head_pose_time.h) is a return address
inside the game's mapped image and a prediction under 5 ms either way
(exclusive, both signs). It names no build, so a game update does not turn it
off; if an update moves the caller, the first-sight line names the new return
address. The instant is the latest frame's predictedDisplayTime, taken at the
WaitGetPoses publish point; with no frame yet (or a display time that is not
positive) the call falls back to now + prediction and is counted in `fallback`.
A call that does not reach the owner thread (no live session, a bad origin) is
counted in `failed`.

**Why this and not the guard.** The guard (below, kept as history) covered the
missing tiles with a margin: it told the game a wider frustum, cost about 6% GPU
at h=0.25/v=0 and cropped the extra away. The squares were a lag, not a
frustum: they need head motion, and a steady head shows none. Fixing the lag at
its source costs nothing.

**Reading a log.** `python tools\edvr_log.py --target <store> --tally pose`
tables the `pose gap:` lines by (thread, return address). Elite's game-thread
caller should read a gap of 0.00 ms and an angle of 0.000 deg; a caller that
passes a real prediction, or is outside the image, shows its true lag.

## What EDVR does about it — the cull guard

> **Historical. The terrain guard was removed 2026-10-09** (Sean's decision after
> flight 3); none of its keys exist, and an old line in an `edvr.ini` is carried
> by the installer as "no longer used by this version" and does nothing. This
> section and "Reading the log" below describe the code as it was. The FOV trim
> that rode the guard's stage machine still exists, as `native_fov_trim.h`.

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

---

## 2026-09-23 — the channel flight: it is the tangents, live

Flown on the Pimax Crystal Super (Pimax OpenXR; per-eye frustum
`-1.5293/+1.0324` x `±1.2648`, render 3070x3032) on build
v0.17.0-382-g8ce12926, two sessions. **The Crystal Super reproduces the
bug** — the first Pimax-family rig here that does; the older "this rig
never showed the tiles" note below belongs to the Crystal over PiOpenXR.

Session 051523: `channel = raw`, guard on, from launch — Live at 05:15:58
(+19.4% horizontal), edges clean (trim 0 until 05:16:29; the 10-degree
outer trim afterwards was the pilot's own menu edit). Switched to
`channel = matrix` at 05:21:40 — Live immediately at the same size ask,
so **no target rebuild happened**, and **the black squares came back**.
Session 053407 repeated it with a guard-off baseline: matrix, off,
matrix, raw — squares whenever the guard was off or in `matrix`.

The matrix-period squares with no intervening rebuild decide the model:

- **The culler follows `GetProjectionRaw`, live.** With `raw` the only
  channel lying, the edges were clean; the moment the raw channel went
  honest (matrix mode), the squares returned — at the same target sizes,
  so no re-derivation event can explain it. The cached-frustum model
  (this doc's H3) is refuted, and so is any "wider of the two channels"
  union model.
- **The matrix channel feeds nothing the culler reads.** `matrix` mode
  lied wide on the matrix and the squares showed anyway.

This re-reads the 2026-09-09 legacy-probe `raw`-inert result
(canted-projection.md): that row said lying through `GetProjectionRaw`
alone changed nothing. *(Inference, marked as such: the legacy proxy's
matrix-channel edit was proven live — the fold experiment moved the
rendered picture through it — but its raw-channel edit was never proven
to reach the game at all. Today's flight, on an instrument whose raw
edit demonstrably lands, says the culler does follow that channel. The
09-09 `raw` row should not be cited as evidence again.)*

**What this settles for the patch.** The culler's fov source is the
tangents channel, consumed live — and the game's only symmetric
projection source on that channel is the fov getter `FUN_1404e2f50`
(RVA 0x4E2F50), which folds the four tangents into angle sums of their
magnitudes. Hooking it to emit the per-axis symmetric superset covers
the true per-eye frusta while the renderer (matrix channel) is never
touched: zero extra pixels, no target growth, no crop, and — because
the following is live — immediate effect with live tuning. That is the
build described in the Status block's Next bullet, and it makes the
whole render-wider-and-crop guard unnecessary on rigs where it flies
clean.

One caution carried forward: the only other known consumer of the
getter's outputs is fov-priced LOD (the AstroSurface screen-size gate);
widening shifts subdivision thresholds slightly. Compare LOD/pop against
a guard-off baseline on the patch's first flight.

## 2026-10-09 — the fov getter is not the culler's input

Offline disassembly of Elite build 332841 (FileVersion 332841 / ProductVersion
4.4.1.1; the "332753" label in the entries above is wrong); dumps in
`analysis\decomp\verify_20261009_cull2_*`. **READ** is what the instructions
and the 09-23 logs say; **INFERRED** is what follows from them.

**READ**

- 0x4E2F50 (wrapper slot 25) calls `GetProjectionRaw` at 0x4E2FA2
  (`call [rax+0x10]`, return RVA 0x4E2FA5) and returns `{aspect = recommended
  W/H, atan|t| + atan|b|, atan|l| + atan|r|}`. The first number is an aspect,
  not a tangent, and the vertical sum pairs t with b and the horizontal sum l
  with r; the "r+b / l+t" pairing above was wrong.
- Only out[0] and out[1] are read: by the eye camera setter 0x2878DC0, and by
  the UI scale at 0x2842AE3, k = tan(0.782) / tan(vFOV/2). Widening the getter
  would therefore resize the UI.
- Six `GetProjectionRaw` call sites in three wrappers: slot 25 (0x4E2F50) at
  0x4E2FA5; slot 28 (0x4E29B0) through its helper 0x4E3C50 at 0x4E3C93; slot 24
  (0x4E4270) at 0x4E42FA, 0x4E4351, 0x4E43A6 and 0x4E43F4, which return sizes
  for the UI.
- The slot 28 helper returns fx = W/(|l|+|r|) and fy. They are read by
  0x28219D0 -> 0x2860FA0, a full-screen quad pass.
- From the 09-23 logs, re-read: the getter's read outputs were identical in the
  raw window and the matrix window, so the getter cannot be what changed
  between the squares being absent and present. And the windows did not ask the
  same +19.4%: from 05:16:29 a 10-degree outer trim was on, the ask was
  2554x3032 in both modes, and raw differed from matrix only on the inner edge,
  1.6% of span.

**INFERRED**

- The quad pass behind slot 28 is sky or background.
- The only model that fits the 09-23 windows is a frustum centred on the eye
  axis with half-width (|l|+|r|)/2 taken from Raw, built by a caller not yet
  identified. Static analysis found no culler.

**Ruled out:** the getter as the culler's input, and with it the withdrawn
patch; the "same +19.4% ask" premise. Nothing else is newly ruled out.

**What the test build does.** The runtime sees who calls, because EDVR
implements `GetProjectionRaw` itself and the game's `call [rax+0x10]` lands in
it directly.

- A census, always on, records the game's return address (frame 1, as an RVA in
  the game's image, or "outside") for `GetProjectionRaw`, `GetProjectionMatrix`
  and `GetEyeToHeadTransform`. Frames 2 and 3 come from a stack capture taken on
  the first sight of a new frame 1, on every 16th call of each frame 1 (so a
  caller that turns up later behind a known frame 1 is named within moments;
  calls not captured are counted as unsampled), and on every call of the first
  site while the probe needs it. The log has a `projection callers:` line on first sight
  of each distinct (method, frame 1, frame 2), and count summaries at 30 s,
  every 5 minutes and before every probe change.
- `advanced.cull_probe` answers the selected callers of `GetProjectionRaw` with
  the per-axis symmetric superset of what they would have been told, jitter
  shift included. Groups: camera is frame 1 0x4E2FA5 with frame 2 0x2878E1B; ui
  is 0x4E2FA5 with frame 2 0x8D269A; sky is 0x4E3C93; sizes is 0x4E42FA,
  0x4E4351, 0x4E43A6 or 0x4E43F4; other is anything not matched, including
  0x4E2FA5 with any other frame 2; all is every caller. It acts only with the
  cull guard off and only on build 332841 (PE stamp 1788384820, image
  104894464), and the log says when it does not.
- The getter's other half (READ, 2026-10-09, new disassembly): the eye camera
  takes its HORIZONTAL extent from out[0], the aspect, which the getter gets
  from its own GetRecommendedRenderTargetSize call (`call qword ptr [rax]` at
  0x4E2FBC, two bytes, so the return is expected at 0x4E2FBE; the census shows
  the real value) and only its VERTICAL extent from `GetProjectionRaw` (out[1]).
  So on a vertically symmetric headset a `GetProjectionRaw` lie cannot widen a
  centred (aspect, vFOV) frustum at all, and a probe that touched only
  `GetProjectionRaw` would read "no group matters" whatever the culler does.
  The probe therefore also answers that one call: under a selected group (all,
  camera, ui, other; the same frame-2 table as the getter's raw site) it is told
  the height kept and the width height * A to the nearest even number, with A
  the larger over the eyes of max(|l|,|r|) / max(|t|,|b|) from the true located
  tangents (before any lie or jitter), which makes the camera's centred frustum
  the per-axis symmetric superset. Every other asker, the render-target
  allocation first, is told what it was, bit for bit. The log says it once per
  change: `cull probe: <group> also told aspect A' (true A) at the fov getter`.
- H-cam (INFERRED): the culler is a centred frustum built from the camera's
  (aspect, vFOV) fields. It fits the old guard flying with v=0: that guard's
  horizontal widening reached the game as a bigger render size, i.e. aspect.
  FUN_13ACC40, which builds a double-precision view-projection from those
  fields, is the candidate consumer (struck later the same day, round 4: it is
  "FSSRenderingComponent"; see the rounds 3-5 entry below).
- `advanced.cull_probe = cycle` drives the groups itself so the result is a
  number and not an impression (the black squares flicker as the head moves, so
  20 s windows cannot be judged by eye): off, all, off, camera, off, ui, off,
  sky, off, sizes, off, other, in 2.0 s windows, the first 30 frames of each
  dropped, counting the planet-terrain draws each eye gets (the colour pass's
  patch VS, 72BDD292154158AD, the draws the planet patch motion already keys
  on) and their summed index counts, since tile LOD varies. A culler whose
  frustum widens admits more tiles at the edges, so the group that feeds it
  should show more draws and indices than its neighbouring off windows.
  `edvr_log.py --tally cull` tables the windows and the paired differences,
  leaving out windows in which the head moved faster than 20 deg/s.
- `advanced.cull_probe = measure` is the positive control. The old guard
  (`fix.cull_guard = symmetric`) is proven to remove the squares, and a counter
  that cannot see its terrain draws rise when the guard goes live is blind: its
  silence under the lie probe would mean nothing. Measure is the same windows
  and counting with no lie, each window labelled by the guard's stage as the
  runtime last told this half (off when not configured, otherwise waiting,
  adopting, live; the channel does not separate waiting from inert), and a
  window dropped when the stage changes under it. It keeps counting while the
  guard runs: only the lying stands down. `--tally cull` prints live minus off
  (the difference of the stage means, with the standard error of the
  difference), leaving out the staging windows and those the head moved in.
  Lines: `cull cycle: measure[guard live] window N: frames F, terrain draws L/R
  mean a/b, indices L/R mean c/d, head x.x deg/s`.
- The guard arms live (READ, native_cull_guard.h `beginFrame`): any change of
  its settings, off to symmetric included, resets it to Off, and from Off it goes
  to Adopting as soon as the scene is ready (it asks the game for bigger render
  targets while still telling the truth), then to Live once both eyes submit at
  the new size; terrain-culling.md measured the game's rebuild at about 14 s
  mid-session. So the positive control needs no relaunch, and an empty
  `cull_guard_headsets` runs it everywhere. Not yet flown on the native runtime:
  if the log has not said live 60 s after the switch, relaunch with the guard on.
- The same flight carries the canted-display test keys (canted-projection.md).

## 2026-10-09, rounds 3-5 — static eliminations, and the mono camera

Offline disassembly of Elite build 332841 again (dumps in `analysis\decomp\`),
then a test build. Nothing here is flown. **READ** is what the instructions say;
**INFERRED** is what follows.

**READ**

- The controller tick 0x107346A (the census's off-thread caller, 0x1073470)
  reads only out[1] of `GetProjectionRaw`, into controller+0x30.
- Eye cameras build their frustum from the kind-5 matrix; 4F3770 inverts
  +0x1B0. It is exact, not a centred (aspect, vFOV) frustum.
- FUN_13ACC40, the double-precision view-projection builder named as H-cam's
  candidate consumer, is "FSSRenderingComponent".
- A mono camera exists, built at 2871D30 (kind 0/3). Its aspect field is B+0x80,
  written only by FUN_28634E0 (rcx = B, edx = width, r8d = height, xmm3 = the
  minimum aspect, a float* out at [rsp+0x28] at entry, written conditionally),
  and defaults to 16/9. The getter at 0x2841190 reads a camera's aspect
  (`F3 0F 10 41 70 C3`, `movss xmm0,[rcx+70h]`, `ret`); the mono filler calls it at
  0x2871D86 (`call qword ptr [rax+0x40]`), which returns to 0x2871D89 and stores
  the result at 0x2871D92. No terrain reader of the mono camera was found.
- The AstroSurface chain uses camera 0's pose, a fov setting in degrees and the
  viewport height (a LOD gate), as the earlier entry said.
**Demoted, not ruled out:** the 09-23 verdict that the culler follows
`GetProjectionRaw` and not the matrix. It was a pilot's judgement of flickering
squares at the periphery, with a 10-degree outer trim on and a 1.6% difference
between the windows (the 10-09 entry above). UNRELIABLE: nothing is built on it.

**INFERRED**

- The probe's eye groups (all, camera, ui, and the aspect lie with them) cannot
  be expected to move an eye camera's frustum, which does not use those values.
  If H-cam holds, the carrier is another camera, and the mono camera is the one
  that exists.

**What the test build does.** `advanced.cull_probe = mono` and the cycle's last
window multiply the mono camera's aspect by 1.30. `all` keeps its meaning
(`GetProjectionRaw` and the aspect lie only).

- The hook is on the getter at 0x2841190. It goes in lazily, on the first frame
  `cull_probe` is cycle, measure or mono, once, through a gate: PE stamp
  1788384820, image size 104894464 and the bytes of both functions. On any
  mismatch nothing is patched and one line says why: `cull probe: mono windows
  multiply the mono camera's aspect by 1.30 (hook live | hook inert: <why>)`.
- The detour calls the original and returns its value untouched, except when the
  game's return address is 0x2871D89 AND a mono window is active: then
  aspect * 1.30. It reads the return address because the relay jumps into the
  detour. Every other caller, and every call while no window is active or the
  key is off, gets the original bit for bit (with the key off the relay does not
  enter the detour at all).
- Two observe-only records, written at the frame boundary: the first sight of
  each distinct return address that calls the getter (16 at most; the line
  names the mono filler), and one line per call of FUN_28634E0 (50 at most:
  width, height, the minimum aspect, `*out` before and after, the return
  address). The writer's hook calls the original with its arguments and returns
  its result.
- The cycle is now 14 windows: off, all, off, camera, off, ui, off, sky, off,
  sizes, off, other, off, mono. `mono` is paired with the next cycle's first off
  window like any group. With the hook live every window line ends `, mono reads
  N`, the calls from 0x2871D89 over its counted frames; `--tally cull` tables
  it by group and prints the hooks' own lines. A steady `mono` run writes no
  windows, only the hooks' lines.

**Reading it.** A mono window whose terrain draws and indices rise against its
two off neighbours names the mono camera's frustum as the culler's input. A
mono window with `mono reads` above zero and no rise retires the aspect read
through that one call, not the mono camera: the observe lines list every other
caller of the getter and every call of the writer, which is where to look next.
A mono window with `mono reads` 0 says nothing (the filler was not reached in
that scene). The positive control (`measure`, flight A) still has to show live
above off first.

## 2026-10-09, round 6 — the squares follow the head: two poses, and the test build

Offline disassembly of Elite build 332841 (`analysis\decomp\verify_20261009_cull6_*`), then a test build. Nothing here is flown. **READ** is what
the instructions say; **INFERRED** is what follows from them.

**READ**

- The function at 0x4E3690 hands Elite a head pose. Its entry sets `[r9] = 0`, loads the IVRCompositor (`[rcx+0x108]`) and, at 0x4E36DE, tests
  arg6 (`[rbp+0xE0]`): non-zero jumps to 0x4E36F4. For arg6 = 0 it tests the latch `[rsi+0x111]` at 0x4E36E7 and, when it is clear, branches at
  0x4E36EE to 0x4E384F.
- 0x4E36F4 tests the latch again: set goes to 0x4E37E8 (the cached pose); clear falls through to `call [rax+0x10]` at 0x4E3715, WaitGetPoses.
  So the render thread (arg6 = 1) always takes WaitGetPoses' pose, and the game thread (arg6 = 0) takes it only while the latch is set.
- 0x4E384F loads the IVRSystem (`[rsi+8]`), reads its vtable slot 0x50 (GetDeviceToAbsoluteTrackingPose, slot 10) into rbx, forms the prediction
  as arg3 (a double) less the result of a call through `[rax+0x50]` at 0x4E3860, narrows it to float (`cvtsd2ss xmm2` at 0x4E387B) and calls
  at 0x4E387F (`FF D3`). The return address is 0x4E3881. The prediction is about 0 s in practice, so the pose is "now". A missing compositor
  (`test rcx,rcx` at 0x4E36D5) also goes to 0x4E384F.
- The branch at 0x4E36EE is `0F 84 5B 01 00 00`: `je` with displacement 0x15B, ending at 0x4E36F4, so its target is 0x4E384F. The bytes at
  0x4E36E8..0x4E36F7 are `BE 11 01 00 00 00 0F 84 5B 01 00 00 80 BE 11 01` (the tail of the `cmp` at 0x4E36E7, the branch, the start of the `cmp` at
  0x4E36F4). Both opcode bytes are in the aligned word 0x4E36E8..0x4E36EF.
- The caller that asks with arg6 = 0 is the controller tick on tid 24212, which reaches 0x4E3690 through V+0x60 (round 6 trace).

**The patch.** Replace `0F 84` with `90 E9`. The displacement that follows is untouched, so the result reads `nop` at 0x4E36EE and `jmp rel32` at
0x4E36EF, ending at 0x4E36F4 with the same 0x15B: the same target, 0x4E384F. The branch becomes unconditional, so an arg6 = 0 caller never reaches
0x4E36F4's latch test and never takes the cached pose; arg6 != 0 callers jump over this code at 0x4E36E5 and are unaffected.

**INFERRED**

- The planet-terrain culler takes the game-thread camera (H-lag): its tiles are chosen with a pose at least one frame older than the drawn one.
  The pilot's pattern (squares only while the head moves, filled in when it is steady) is what a lag of that size would do and a static frustum
  would not.

**What the test build does.**

- *Instrument, always on.* GetDeviceToAbsoluteTrackingPose records, before the hop to the owner thread, the caller's thread, its return address as an
  RVA in the game's image and the prediction. After the locate it takes the located instant less the latest frame's predictedDisplayTime (ms), the angle
  between the pose handed back and the pose that frame was drawn with, and the head's angular speed from the render pose. Per (thread, return RVA)
  and 2.0 s window, flushed from the WaitGetPoses publish point (a mode change closes the window at once):
  `pose gap: mode <m> tid T calls n from exe+0x… prediction p ms target-minus-display mean a ms (min b max c) angle-to-drawn mean d max e deg head f deg/s
  waitgetposes w failed k`, with ` fallback n` appended when a call could not be formed. Failed locates, including the silent busy-gate path, are in
  `failed`. `from` reads `outside` or `?` for a caller that is not in the image or was not captured.
- *Switch.* `advanced.cull_pose` is read by the graphics half and carried to the runtime in `EdvrNativeFrameOutput::cullPose` (version 8, under the
  same hand-copied-DLLs rule as the others: an older runtime reads off). The runtime acts on it: the call returning to 0x4E3881, on build 332841, is
  located at the latest frame's display time (`display`, `display_direct`) or one period later (`next`, `next_direct`) instead of now + prediction;
  every other caller is unchanged, bit for bit. A Display or Next instant that cannot be formed (no frame yet, no positive period) falls back to
  today's answer and is counted.
- *Patch.* The `_direct` modes make the graphics half write the 2-byte patch above as one interlocked eight-byte store over 0x4E36E8..0x4E36EF, gated
  on the PE stamp, the image size and the six original bytes, from its frame boundary (the owner thread inside WaitGetPoses, which every pose call
  hops to as well, so it is never inside one), and put the original bytes back when the mode leaves `_direct`. A refusal is final until the mode leaves
  `_direct`.
- *The line a change writes* (graphics log): `cull pose: <mode> -- Elite's game-thread head pose is located at <now+prediction | the frame's display
  time | display time + one period>; latched-pose bypass <on|off|refused: why>`. On another build a time mode adds `(standing down: not build 332841)`
  after the instant it falls back to.

**Reading it.** Under `off` the game thread's gap should be near the time since the last frame began (several ms) and its angle should grow with
head speed (r near +1 in `--tally pose`); display should read 0 ms and next one period. If squares go with a smaller angle, the culler is on that
pose; if the squares do not change under any mode, it is not (and the instrument has still said how stale the pose was).

## 2026-10-09, rc flight — residual squares on rapid turns (H-onef), and the second test build

**Flight.** Main at v0.18.3-164-gf9597bd3 (the head-pose fix shipped as behaviour), Crystal Super on Pimax OpenXR, the Frontier copy. Gaze switches no
longer show squares. On approach to a planet, a RAPID head turn still shows black terrain squares at the OUTER edges. The runtime log's `pose gap:` lines
for Elite's caller (tid 24212, exe+0x4E3881) say the shipped `display` is working: gap 0.00 ms, mean angle to the latest frame's render pose 0.003 to
0.029 deg, fallbacks only while loading. One window has a max angle-to-drawn of 2.8 deg. That figure is against the latest frame's render pose, so it is
not the lag between what the game thread is handed and what is drawn when it is used.

**H-onef (untested).** The game thread prepares frame N+1 while the render thread draws N. While N renders the latch at [W+0x111] is set, so the game
thread's call does not reach 0x4E384F and Elite reuses the pose cached for N (round 6). That pose is drawn one frame later, as N+1's cull camera. Located
at D_N (display time), it is one display period behind D_N+1: about 11 ms, which is 2.8 deg at 250 deg/s and nothing at a gaze switch. The squares then
sit on the leading outer edge of a fast turn. Two candidate fixes, both in the test build: answer at D_N + one period (`next`), and keep the game thread
off the latched pose so the call is always answered fresh (`display_direct`, `next_direct`).

**The test build** (not flown; TEMPORARY, to be removed or shipped as behaviour when the arc closes).

- `advanced.cull_pose` = `display` (the default, exactly what ships; an unset key changes nothing) | `now` (the pre-fix answer, a control) | `next` |
  `display_direct` | `next_direct`. Only Elite's own "now" calls move (return address in the game's image, prediction under 5 ms either way); the
  freshness guard (one period, at most 50 ms) and the TIME_INVALID retry stay, and `next` also falls back to now + prediction when the period cannot
  be added or a reference-space change falls between the display time and the target. `next` reuses `nextPredictionTime`, the arithmetic the game-pose
  array uses. The mode travels in `EdvrNativeFrameOutput` version 6 (`cullPose`); no build gate for the time modes.
- The `_direct` modes restore the round-6 patch: `0F 84` to `90 E9` at RVA 0x4E36EE, one atomic aligned eight-byte store over 0x4E36E8..0x4E36EF at the
  frame boundary, gated on the PE stamp (1788384820), the image size and the six bytes, restored on leaving a `_direct` mode; refused elsewhere.
- Graphics log, on a change: `cull pose: <mode> -- Elite's game-thread head pose is located at <now+prediction | the frame's display time | display
  time + one period>; latched-pose bypass <on|off|refused: why>`. A key left unset writes nothing.
- Runtime log, `pose gap:` (every 60 s; a mode change closes the window): `pose gap: mode <m> tid T calls n from exe+0x… prediction p ms
  target-minus-display mean a ms (min b max c) angle-to-drawn mean d max e deg angle-to-next-drawn mean g max h deg head f deg/s waitgetposes w failed k`.
  **angle-to-next-drawn** is new: the angle between the orientation each game-thread caller (thread, return RVA) was last handed during frame N and the
  render orientation published for N+1, measured at the N+1 publish; one orientation per caller, no allocation. `--tally pose` tables it per mode and
  caller, with the largest window.

**Verdict rule.** Same approach and turns, one run each of `display`, `next`, `next_direct`. If H-onef holds, `display` reads angle-to-next-drawn near
head speed x one period (mean tens of hundredths of a degree and a max of a few degrees on a fast turn), `next` reads near 0, and the squares go or
shrink under `next`. If angle-to-next-drawn is near 0 under `next` and the squares remain, H-onef is wrong (record `ruled out: H-onef, because ...`)
and the culler is not working from this pose at that instant. If it does not fall under `next`, the cached pose is still reaching Elite: read
`next_direct`, which closes it. Nothing is ruled out yet.
