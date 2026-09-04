# Code review: the temporal pass's motion path, and why the text is soft

An adversarial read of the motion-vector chain on branch `claude/temporal-aa`,
looking for the cause of the field's consistent report that cockpit, panel and
menu TEXT is softer with the temporal pass than without it.

**Build reviewed:** `02e2582` ("temporal aa: the frame rate in the totals
line"), 16 commits past main. Nothing was edited, built or run; the evidence is
the source, NVIDIA's SDK headers in `third_party/ngx/include`, and the flight
logs in
`C:\Steam\steamapps\common\Elite Dangerous\Products\elite-dangerous-odyssey-64\edvr_logs`.

**A warning about the newest log first.** `edvr_vr_20260904_060044.log` carries
this build's version line (`v0.13.3-22-g02e2582`) but is NOT a flight of the
pass: at 06:00:47.330 it says

> temporal aa STANDING DOWN: d3d11.dll exports no temporal pass (mismatched pair?)

so that session ran the openvr half of this build against some other d3d11.dll
(the two-installs hazard). The useful flights of this build are the graphics
logs of 2026-09-03: `edvr_gfx_20260903_152846.log` (DLAA, then DLSS at 65%,
Pimax) and `edvr_gfx_20260903_152607.log` (the same, 5424x5356). Read those,
not the newest.

---

## The chain in one screen

For an implementer who has not seen the code:

1. `temporalAaFrameBoundary` (`src/openvr/temporal_aa.cpp:195`) runs at the END
   of `WaitGetPoses`, after the system hook's boundary. It advances the frame
   counter, draws frame *n*'s Halton (2,3) offset `(jx, jy)` in render pixels,
   converts it to a tangent shift with `temporalJitterToTangents`
   (`src/common/temporal_math.h:66`) and hands that to `systemHookSetJitter`.
2. The system hook adds the shift to all four tangents in both the raw thunk
   (`src/openvr/system_hook.cpp:465-473`) and the matrix receiver
   (`src/openvr/system_hook.cpp:598-618`), so the game renders frame *n*
   displaced by `(+jx` right`, +jy` down`)` pixels.
3. At Submit, `temporalAaTreat` (`src/openvr/temporal_aa.cpp:290`) collects the
   UNJITTERED tangents (`systemHookEffectiveTangents`), the head's rotation
   delta and per-eye translation term, the planes, and calls `edvrTemporalAa`.
4. `temporalInner` (`src/d3d11/temporal_pass.cpp:804`) finds this eye's scene
   depth through `depthProbeSceneDepth`, and either runs its own history
   (`main`) or, with flag bit 1, copies the colour, dispatches the `mv` entry
   (`src/d3d11/temporal_pass.cpp:213-248`) to write motion vectors and a depth
   copy, and calls `dlaaEvaluate` (`src/d3d11/dlaa.cpp:217`).
5. Order at the door: temporal → cull guard crop → supersample resolve →
   sharpen (`src/openvr/compositor_hook.cpp:1511-1516`).

---

## Findings, ranked by likely contribution to soft text

### F1 — CONFIRMED. NVIDIA's history is reset on EVERY evaluation, so DLSS and DLAA never accumulate anything.

**Where.** `src/d3d11/temporal_pass.cpp:1294`

```
const bool resetHist = (flags & 1u) != 0 || !e.haveHistory;
```

and `src/d3d11/temporal_pass.cpp:1396-1402`

```
if (ran && usedDlaa) {
    // The trained pass's frame goes out; the pass's own history is
    // marked broken so a switch back starts afresh.
    e.haveHistory = false;
```

`e.haveHistory` is set true in exactly one place, `temporal_pass.cpp:1405`,
which is the pass's OWN branch. On the trained path it is set false at the end
of every frame (1399) and cleared again on any reset (1084). It is therefore
false at line 1294 on every DLAA/DLSS frame, `resetHist` is always true, and
`dlaaEvaluate` is called with `reset = true` for every eye of every frame
(`temporal_pass.cpp:1295-1296`), which becomes `ep.InReset = 1`
(`src/d3d11/dlaa.cpp:325`) and then
`NVSDK_NGX_Parameter_Reset`
(`third_party/ngx/include/nvsdk_ngx_helpers.h:230`).

The SDK's own comment on that field
(`third_party/ngx/include/nvsdk_ngx_helpers.h:147`) reads:

> `int InReset; /* Set to 1 when scene changes completely (new level etc) */`

**Mechanism, in plain words.** Every frame the pass tells NVIDIA that the scene
has completely changed. DLSS throws away its accumulated buffers and
reconstructs the frame from the single input it has. The jitter, the motion
vectors and the depth are then all decoration: there is no history for them to
register.

**Failure scenario.** Under `temporal_aa = dlaa` the network runs as a spatial
anti-aliaser on one 5424x5356 frame — which is why the Pimax verdict was
"amazing, no shimmering text": at that density the input is already good and
nothing ghosts, because nothing accumulates. Under `temporal_aa = dlss` the
network must invent the missing 35% of linear resolution from ONE frame, with
no earlier jittered samples to draw on. A single-frame neural upscale of text
is exactly what soft text looks like. This is the single best explanation on
the table for "DLSS text is fuzzy while DLAA is not".

**Fix.** Track the trained pass's own continuity, separately from the own
pass's history flag. Concretely: give `EyeState` a `dlHaveHistory` (or reuse
`e.dlOutW != 0` as the "the feature has evaluated at least once at this size"
witness) and compute

```
const bool resetHist = (flags & 1u) != 0 || !e.dlHaveHistory;
```

setting `e.dlHaveHistory = true` after a successful `dlaaEvaluate`, and false
whenever the feature is (re)created at `temporal_pass.cpp:1230-1252`, whenever
the pass falls back to its own history, and whenever `flags & 1u` arrives (a
withhold, an eye's first frame). Keep `e.haveHistory = false` at 1399 — the own
pass's history really is broken while the trained one runs — but stop reading
it for the trained pass's reset.

**Verify.**
* Desk: add a `resets` out-parameter to `dlaaTotals` (`src/d3d11/dlaa.h:53`),
  count evaluations issued with `InReset != 0`, and in `tools/smoke/smoke.cpp`
  call the export eight times with flags `2u` (no reset bit) after one call
  with `1u | 2u`; assert `resets == 1`, `evaluations == 9`.
* Field: extend the `dlaa totals` line
  (`src/d3d11/temporal_pass.cpp` totals path / `src/d3d11/dlaa.cpp:379`) with
  "… of which N started NVIDIA's history afresh". On a good build N is a handful
  per session; today it will equal the evaluation count.

---

### F2 — CONFIRMED. The optimal-settings query is asked of the wrong parameter block, so it always fails and the DLSS quality mode is chosen blind.

**Where.** `src/d3d11/dlaa.cpp:202` allocates the working block with
`NVSDK_NGX_D3D11_AllocateParameters(&g_params)`. `src/d3d11/dlaa.cpp:254` and
`:264` then call `NGX_DLSS_GET_OPTIMAL_SETTINGS(g_params, …)`.

The SDK header says exactly why that cannot work
(`third_party/ngx/include/nvsdk_ngx_helpers.h:78-86`):

```
void *Callback = NULL;
NVSDK_NGX_Parameter_GetVoidPointer(pInParams, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &Callback);
if (!Callback)
{
    // Possible reasons for this:
    // - Installed DLSS is out of date and does not support the feature we need
    // - You used NVSDK_NGX_AllocateParameters() for creating InParams. Try using NVSDK_NGX_GetCapabilityParameters() instead
    return NVSDK_NGX_Result_FAIL_OutOfDate;
}
```

It returns before writing any out-parameter, so `optW/optH/minW/minH/maxW/maxH`
keep the zeros they were initialised to at `dlaa.cpp:251-252`. The flight logs
confirm it, verbatim:

> dlaa: the feature is created for eye 1 at 5424x5356, DLAA (the runtime's
> **optimal render size 0x0**, sharpness 0.00 …)

> dlss: … the balanced mode, **whose render range the runtime names as
> 0x0..0x0**

**Mechanism.** The ladder at `dlaa.cpp:258-270` is meant to step the quality
value down until the game's actual render size sits inside the mode's range.
With min = 0 and max = 0 the guard `w >= minW && h >= minH && w <= (maxW ? maxW
: w)` is trivially true, so the loop always takes `ladder[start]` on its first
iteration and the range check is dead code.

**Failure scenario.** On the Pimax flight the game rendered 3525x3481 into
5424x5356 — 64.99% per axis, a hair under the 0.66 threshold at `dlaa.cpp:261`
— so the feature was created as `Balanced`, whose own ratio is 58%. The
PerfQualityValue is not only a label: the runtime uses it to pick the render
size it expects and, on several driver generations, the DLSS preset. Declaring
a mode whose expected input is smaller than the input actually handed over is
out of spec, and preset selection is one of the few knobs that visibly changes
how DLSS treats thin high-contrast strokes.

**Fix.** Keep the capability block. In `dlaaAvailable`
(`src/d3d11/dlaa.cpp:190-208`) `caps` is fetched and dropped; store it in a
file-scope `NVSDK_NGX_Parameter* g_caps` and pass `g_caps`, not `g_params`, to
both `NGX_DLSS_GET_OPTIMAL_SETTINGS` calls. Then check the result: if it fails,
say so once in the log instead of printing zeros as if they were an answer.
With real numbers in hand, prefer the mode whose `optW` is nearest the actual
render width rather than the ratio ladder, and refuse (fall back to the own
pass) when the input is outside `minW..maxW`.

**Verify.** Desk: in `tools/smoke/smoke.cpp`'s dlaa block, print the
`optW/optH/minW/minH/maxW/maxH` the pass obtained; on an RTX machine with the
runtime present they must all be non-zero for a 600x456 target. Field: the
"dlss: the feature is created" line must stop saying `0x0..0x0`.

---

### F3 — CONFIRMED (by this project's own earlier work). Under DLSS proper, most of the panel text's softness is not a motion-vector defect at all.

Elite draws the cockpit HUD and the panels into offscreen interface surfaces
whose size scales with the INTERNAL render resolution (the target-indicator
surface was measured at 0.4498 x 0.3726 of the render size), then composites
those surfaces onto panel geometry as textures. When Elite's HMD Quality is
lowered to feed DLSS — 3525x3481 against a 5424x5356 unit-quality size, the
Pimax flight — the text is RASTERISED at 65% of the linear size. No upscaler,
jitter or motion vector recovers glyph detail that was never drawn: the text
can only ever be as sharp as the surface it was drawn into.

**How to tell F3 apart from F1/F4/F5 in the field.** Fly two intervals at the
SAME output size and compare the text:

* `temporal_aa = dlaa` with Elite's HMD Quality at 1.0 (input == output; the
  interface surfaces are full size);
* `temporal_aa = dlss` with HMD Quality at 0.67 (the same output size, the
  surfaces two-thirds the size).

Any softness present in the first is the pass's; the extra softness in the
second is F3 plus whatever F1 leaves behind. Do this AFTER F1 is fixed, or the
comparison measures the reset, not the surfaces.

**There is no fix inside EDVR.** State it in the README where `temporal_aa =
dlss` is documented: DLSS buys frame time, not text.

---

### F4 — PLAUSIBLE. The jitter's phase rests on an unverified assumption about when the game reads its projection.

**Where.** `src/openvr/compositor_hook.cpp:1940-1954`. The comment states the
assumption outright:

> The system hook's frame boundary: the cull guard's lie switches on and off
> HERE, after the game is released from WaitGetPoses and **before it queries
> this frame's projections**

`temporalAaFrameBoundary` runs last in that sequence
(`compositor_hook.cpp:1954`) and sets frame *n*'s jitter. The pass then hands
`s.jx/s.jy` to the shader and to NGX at Submit
(`src/openvr/temporal_aa.cpp:422-427`, `src/d3d11/temporal_pass.cpp:1296`).

**Mechanism.** The cull guard's lie is CONSTANT across thousands of frames, so
the field verification of the guard's crop proves the tangents and the matrix
agree — it does not prove the game re-reads the projection after the boundary
rather than before it. The jitter changes every frame, so this ordering is now
load-bearing in a way it never was. If Elite reads its projection for frame *n*
during the update phase that precedes its `WaitGetPoses` — or caches it from
the previous frame — the colour carries frame *n-1*'s offset while both the
pass's `jit.xy` and NGX's `InJitterOffset` describe frame *n*.

**Failure scenario.** Consecutive Halton (2,3) offsets in this sequence differ
by up to 0.81 px (computed: the largest consecutive step in the eight-frame x
sequence is 0.375 → -0.4375). A one-frame phase error therefore misplaces every
sample by up to 0.8 px, every frame, in a direction that changes each frame.
DLSS would average the misplacement across the sequence — which is a blur, and
a stable one, matching the report exactly ("soft, but not shimmering").

**Fix (measure first).** Do not change the ordering blind. Instrument it: the
system hook already counts slot 1 and slot 2 calls (`edvr_selftest_system_hook`
publishes them, `src/openvr/system_hook.h:160-163`). Record the counters at the
boundary and again at the frame's FIRST Submit, and print once a session:

> temporal aa: the game asked for its projection N times between the boundary
> and the first submit, and M times between the last submit and the next
> boundary (N should be the larger; if it is not, the jitter is a frame late).

If the answer says the game reads before the boundary, the robust fix is to
stop guessing: have `system_hook` latch, per eye, the `(jitDx, jitDy)` it
actually answered with on the most recent projection query, publish it
(`systemHookJitterAnswered(eye, float*)`), and have `temporalAaTreat` convert
that back to pixels and pass THAT to the pass and to NGX instead of `s.jx/s.jy`.

**Verify.** The log line above, on one docked flight. It costs two counters and
one line.

---

### F5 — PLAUSIBLE. The jitter's sign into NGX has never been checked against NVIDIA's convention, on either axis.

**Where.** `src/d3d11/dlaa.cpp:321-322` passes the pass's `(jx, jy)` through
unmodified.

**EDVR's convention, derived from the code.** `temporalJitterToTangents`
(`src/common/temporal_math.h:66-71`) computes `dx = -jx*(r-l)/w`, `dy =
+jy*(b-t)/h` and the hook adds `dx` to `l` and `r`, `dy` to `t` and `b`
(`src/openvr/system_hook.cpp:469-472`). Working the matrix formula the receiver
uses (`system_hook.cpp:610-614`: `m11 = 2/(b-t)`, `m12 = (b+t)/(b-t)`) through
to viewport rows gives, exactly:

* content moves RIGHT by `jx` columns;
* content moves DOWN by `jy` rows.

That is a self-consistent convention, and the runtime's raw tangents on this
rig make it work: the log reads `t=-1.2648 b=+1.2648`, i.e. `t < 0 < b`, so
`b - t > 0` and row 0 corresponds to the `b` tangent, which is the TOP row.
Nothing in the pass assumes `t > b`, which is right.

**What is not known.** `nvsdk_ngx_helpers.h:143` says only "Jitter offset must
be in input/render pixel space". NVIDIA's public Streamline guide says the same
("jitter offset values are in pixel space") and adds the requirement that the
motion vectors exclude the jitter, but neither states the sign. So the y axis in
particular is unverified: pixel rows run downward while the tangent mapping is
written in a Y-up view space, and that is precisely where a flip hides.

**Failure scenario.** A sign error on one axis makes NGX un-jitter the wrong
way. The error is `2*|j|` — up to 1.0 px — every frame, and it averages over the
sequence into a stable blur rather than a wobble. Text goes soft; nothing looks
broken. Note that F1 currently masks this completely: with the history reset
every frame the jitter offset barely matters, so this can only be judged after
F1 is fixed.

**Fix.** Determine it, then hard-code it with a comment naming the measurement.
Do not guess.

**Verify (the desk test that settles it).** In `tools/smoke/smoke.cpp`, beside
the existing dlaa block:

1. Build a 400x304 R8G8B8A8_UNORM source containing a single one-pixel-high
   white horizontal line on black at row 152 — but drawn SUB-PIXEL, i.e. for a
   frame whose jitter is `jy`, put `1 - frac` of the energy in row
   `152 + floor(jy)` and `frac` in the next row down, `frac = jy - floor(jy)`.
   That is what a jittered rasteriser would produce.
2. Evaluate 16 frames through the export with `motion = 0` (no delta), no
   depth, `flags = 2u` after an initial `1u | 2u`, passing `(0, jy)` as the
   jitter each frame from `temporalJitter`.
3. Read back column 200, rows 148..156.

Expected with the CORRECT sign: the energy collapses back onto row 152 — the
peak sample is within a few 1/255ths of the drawn peak and rows 151 and 153 are
near black. With the sign inverted the same test spreads the energy over about
two rows with roughly half the peak. Repeat with a vertical line and `(jx, 0)`
for the x axis. Both axes must be tested; they can be wrong independently.

---

### F6 — CONFIRMED (by construction). The motion vectors describe the HEAD only. The ship's motion through the world is not in them at all.

**Where.** The delta handed to the `mv` shader is `deltaHead` — the runtime's
head-pose rotation delta (`src/d3d11/temporal_pass.cpp:1089-1094`) — and the
translation term is `temporalHeadTranslation`
(`src/common/temporal_math.h:153`), built from the two render poses and the
eye-to-head offset. Both describe where the player's head went in the room.

**Mechanism.** Elite's camera is bolted to the ship. The cockpit and its panels
are static in eye space except for head motion, so head-derived vectors are
right for them — which is the good news for text. Everything OUTSIDE the canopy
moves with the ship's own rotation and translation through the world, and the
pass reports that motion as zero.

**Failure scenario.** In supercruise, in a turn, or on approach, every world
pixel carries a large screen-space velocity that the MV texture denies. The own
pass survives it because the variance clip rejects the mis-registered history;
DLSS has no such clamp and is documented to trust the vectors, so it ghosts or
starves in its own way. World-anchored HUD elements — the target reticle,
wireframes, the compass — are drawn in world space and are hit by the same
thing.

**Fix.** There is no cheap complete one; the honest options are (a) keep the
own pass as the default and treat DLSS as the render-scale engine only, which
is what `docs/anti-aliasing.md`'s "What the plumbing opens" already says; or
(b) build the world's motion from the game's own view rows, which the pass
already captures (`advanced.temporal_aa_motion = camera`, cand[2] in the
instrument), and use the head's delta only for pixels whose depth says
"cockpit". The second is a real feature, not a fix.

**Verify.** The registration instrument already measures it: the "camera"
candidate's clip share against "head with depth" during flight rather than
docked. Every interval in the logs read so far was docked.

---

### F7 — CONFIRMED. On the trained path the previous frame's frustum is thrown away.

**Where.** `src/d3d11/temporal_pass.cpp:1103` and `:1144`

```
const bool useHistory = e.haveHistory && haveDelta && tanPrev;
...
memcpy(p.tanPrev, useHistory ? tanPrev : tanNow, sizeof(p.tanPrev));
```

Because `e.haveHistory` is always false on the trained path (F1), `useHistory`
is always false, so the `mv` shader always projects into `tanNow` instead of
`tanPrev`.

**Consequence.** Harmless while the frustum is constant, which it is in every
flight so far. It is not harmless the moment the cull guard re-stages to a new
margin, or during the first frames after a resolution change: the motion
vectors would then silently describe the wrong previous frustum for as long as
the two disagree. Fixing F1 fixes this as a side effect — but only if
`p.tanPrev` is keyed on the trained path's own continuity, not on
`e.haveHistory`. Make that explicit rather than incidental.

---

### F8 — CONFIRMED. The registration instrument cannot tell "the depth is doing nothing" from "the head did not translate".

**Where.** The depth term is `dp = dp * z + tv` (`temporal_pass.cpp:162`,
`:234`). When `tv` is zero, scaling `dp` by `z` does not move the projected
point at all — projection is scale-invariant — so the depth candidate and the
rotation-only candidate are BIT-IDENTICAL. The same is true when `zr` reads
zero: the guard at `:160` and `:232` leaves `dp` untouched.

The logs show both faces of this. In `edvr_gfx_20260903_141537.log` every
interval reads

> head, rotation only clipped 9.7% … head with depth clipped 9.6%

while in `edvr_gfx_20260903_140531.log` at 14:07:32 the same instrument reads

> head, rotation only clipped 15.6% … head with depth clipped 12.1%

Identical numbers are evidence of nothing. The speed buckets
(`temporal_pass.cpp:631`, thresholds at `:570-571`) are graded on head ROTATION
in degrees, which is the one quantity the depth term does not care about.

**Fix.** Add the head's TRANSLATION between the two poses — `|t_now - t_prev|`
in millimetres, which `temporalHeadTranslation` already has the inputs for — to
the registration line, and bucket the depth-vs-rotation comparison by it. A
line that says "over intervals where the head moved more than 2 mm a frame,
depth clipped X% against rotation-only's Y%" is a verdict; today's line is not.

**Verify.** One docked flight with a deliberate lean-in / lean-back, comparing
the new buckets.

---

### F9 — CONFIRMED (latent). The colour copy for NGX is hard-coded to `R8G8B8A8_UNORM` while the pass's own format allowlist admits four other families.

**Where.** `src/d3d11/temporal_pass.cpp:1233-1234` creates `e.dlColour` as
`R8G8B8A8_UNORM` unconditionally, and `:1273` copies the game's region into it
with `CopySubresourceRegion`. The allowlist (`viewFormatOf`,
`temporal_pass.cpp:381-406`) admits `B8G8R8A8`, `R10G10B10A2` and
`R16G16B16A16` as well.

`CopySubresourceRegion` requires source and destination to be in the same
format family. `B8G8R8A8_UNORM` → `R8G8B8A8_UNORM` is a different family and is
invalid; the debug layer would say so and a retail runtime drops it silently,
leaving `dlColour` at whatever it last held.

**Cleared for the field, today.** Elite submits `R8G8B8A8_TYPELESS` — measured,
in every "first treated frame" line in the logs — which is the same family, so
the copy is legal on this rig. This is a trap for the next headset or the next
game build, not a live defect.

**Fix.** Create `e.dlColour` in `sd.Format`'s own view format (`viewFmt`, which
the function already has) and refuse the trained path for formats NGX will not
take, saying so once. The same applies to `e.dlOut` at `:1241`, which today
changes the submitted texture's format from `R8G8B8A8_TYPELESS` to
`R8G8B8A8_UNORM` — the own pass deliberately preserves `sd.Format` at `:1060`
and the trained path silently does not.

---

### F10 — CONFIRMED (latent). Under the copy-through path the region rebase is applied to the colour but not to the depth.

**Where.** When the submitted texture refuses a shader view,
`temporal_pass.cpp:1129-1135` rewrites `p.region` to `{0, 0, w, h}` because the
colour has been copied into an origin-based texture. The depth SRV, however, is
still a view over the GAME's full-size target, and both shaders index it with
`Z.Load(int3(region.xy + q, 0))` (`temporal_pass.cpp:156`, `:222`, `:228`).
With the region rebased to zero, a non-zero region origin is lost and the depth
is read from the wrong place.

**Cleared for the field, today.** Every flight logs "Submit bounds: null — the
whole texture is this eye" and "one eye per texture", so the origin is (0,0)
and the two agree. It would bite a double-wide submit on a runtime that also
refuses a view.

**Fix.** Carry the depth's origin separately from the colour's: add a
`depthOrigin` int2 to the cbuffer (there is a spare `w` in `tvUsed`/`tvCand`, or
extend the block) and index the depth with it. Or, more simply, refuse the
trained path and the depth motion when `viaCopy` and the region origin is not
zero, and say so once.

---

### F11 — CONFIRMED (small). `InFrameTimeDeltaInMsec` is never set.

`src/d3d11/dlaa.cpp:315-329` fills the eval params and leaves
`InFrameTimeDeltaInMsec` at its zero-initialised value. The SDK marks it
optional but describes what it is for
(`third_party/ngx/include/nvsdk_ngx_helpers.h:171`):

> helps in determining the amount to denoise or anti-alias based on the speed
> of the object from motion vector magnitudes and fps as determined by this
> delta

The pass already measures the frame rate for its own totals line (build 15,
`02e2582`). Pass it: `ep.InFrameTimeDeltaInMsec = 1000.0f / fps`, or the
measured inter-boundary interval. Cheap, and it is the one remaining eval
parameter with a documented effect on how hard DLSS filters.

Everything else in the eval block checks out — see the cleared list.

---

### F12 — CONFIRMED (instrumentation). The trained path is nearly invisible in the log.

* The "first treated frame" line, which is where the submitted FORMAT, the
  region size and the history format are reported, sits in the own pass's
  branch only (`temporal_pass.cpp:1410-1428`). A DLAA or DLSS session never
  prints it, so a reader cannot tell what format NGX was handed.
* The `temporal aa totals` line reports "0.00 ms per eye on average" during a
  DLAA session because no query slot is acquired when `usedDlaa`
  (`temporal_pass.cpp:1322`) — true, but it reads like a failure. The DLAA
  price is on the separate `dlaa totals` line.
* Nothing anywhere reports the motion-vector texture's contents, the jitter
  actually applied, or the reset count. Every finding above needed source
  reading because the log could not answer.

Add, once per session on the trained path: the submitted format, the MV and
depth formats, the jitter of that frame, and the centre pixel's motion vector
read back through the existing staging machinery.

---

### F13 — CONFIRMED (latent). The jitter is derived from the LEFT eye's tangents and the left eye's region size, and applied to both eyes.

`src/openvr/temporal_aa.cpp:249-256` asks `systemHookEffectiveTangents(vr::Eye_Left, tan)`
and uses `s.renderW/s.renderH`, which are written only for eye 0
(`temporal_aa.cpp:337-340`). The resulting tangent shift is applied to both eyes
by `systemHookSetJitter`.

**Cleared for this rig.** The log gives the two eyes as
`l=-1.5293 r=+1.0324 t=-1.2648 b=+1.2648` and `l=-1.0324 r=+1.5293 t=-1.2648
b=+1.2648` — mirrored, so `(r-l)` and `(b-t)` are identical and the same
tangent shift is the same pixel shift in both eyes. A headset whose eyes have
different tangent widths (or different render sizes) would jitter the right eye
by a different number of pixels than the one reported to the pass and to NGX.

**Fix.** Compute and store the shift per eye, and let
`systemHookSetJitter` take an eye. Also note the first-boundary case at
`temporal_aa.cpp:250`: before any submit the pixel size falls back to
`systemHookRecommendedSize`, which is wrong by Elite's HMD Quality ratio for
the first frame or two.

---

## Cleared / do not fix

* **H1 — jittered vs unjittered tangents.** CLEARED.
  `systemHookEffectiveTangents` (`src/openvr/system_hook.cpp:1453-1460`)
  returns `s->lied[e]` or `s->trueRaw[e]` — the jitter is added downstream, in
  the thunk and the receiver only. So `tanNow` and `tanPrev` are both on the
  unjittered grid, the reprojection maps unjittered-now to unjittered-prev, and
  the motion vectors exclude the jitter, which is what NVIDIA requires and what
  the own pass's Gaussian un-jitter assumes. Both consumers get the same thing.
* **H3 — motion-vector direction.** CLEARED. `temporal_pass.cpp:244-246` writes
  `motion = pp - p`, the pixel's PREVIOUS position minus its current one, in
  render pixels with rows increasing downward. That is the documented
  convention for this class of upscaler (AMD state it for FSR 2 as "from a
  pixel in the current frame to the position of that same pixel in the previous
  frame", in screen-space pixels; DLSS takes the same). `fetchHistoryT` and
  `mv` share the mapping line for line (`:141-143` against `:218-220`,
  `:170-171` against `:242-243`), so the registration instrument and the MV
  texture cannot disagree about direction — which also means the instrument
  cannot CATCH a shared error, hence the desk test in F5.
* **H4 — depth/colour alignment.** CLEARED for the field.
  `depthProbeSceneDepth` is asked for a target of the SUBMITTED texture's exact
  size (`temporal_pass.cpp:997`), so the depth and the colour share a
  coordinate frame and extent by construction; a mismatch yields no depth
  rather than a wrong one. The cull guard's widening applies to both (the game
  renders wide, submits wide). Under DLSS proper the depth targets shrink with
  the render and the census re-picks them — the logs show the pair moving from
  5424x5356 to 3525x3481 within a second, with the "the scene's depth went
  away" line in between. The one residual is F10 (the `viaCopy` rebase).
* **H5 — text without depth.** CLEARED. The cockpit census's own samples of the
  scene pair read real values: `edvr_gfx_20260903_143728.log`, target #11
  (3358x3312, 320 draws/frame) —
  "min 0.00055345, max 0.01951173, centre 0.00154716 (16.16 m); 0 at the far
  plane … 192 at 2..100 m, 64 within 2 m; nearest 0.01951173 = 1.28 m". No
  far-plane holes, and the near cluster at ~1.3 m is the cockpit. Panel text is
  composited onto geometry that has depth, so it inherits it.
* **H6 — MV precision, `MVLowRes`, `MVScale`.** CLEARED.
  `R16G16_FLOAT` (`temporal_pass.cpp:1235`) holds a pixel displacement of a few
  thousand with ~1/1000 px precision near zero, which is well inside what FSR 2
  and DLSS are documented to quantise to internally anyway.
  `MVLowRes` (`dlaa.cpp:280`) says the vectors are at the RENDER size, which
  they are on both paths (the texture is `w x h`, the input size) — a no-op for
  DLAA, correct for DLSS proper. `InMVScaleX/Y = 1.0` (`dlaa.cpp:326-327`) is
  right because the values are already in input pixels.
* **H7 — the eval block's zeros.** CLEARED except F11.
  `InRenderSubrectDimensions` is set to the full `w x h` (`dlaa.cpp:323-324`);
  every subrect base is legitimately zero because the colour, depth and MV
  textures are exactly the region, copied out. `InPreExposure` and
  `InExposureScale` are set to 1.0 (`dlaa.cpp:328-329`), which is also what the
  helper substitutes for zero (`nvsdk_ngx_helpers.h:275-276`).
  `InToneMapperType` 0 is `NVSDK_NGX_TONEMAPPER_STRING`
  (`nvsdk_ngx_defs.h:304`) and is marked "research purposes" in the header.
  `AutoExposure` is not needed because the feature is not created with `IsHDR`.
  `DoSharpening` is deprecated in the header itself (`nvsdk_ngx_defs.h:296`)
  and correctly unused; the sharpening is RCAS at the door, after DLSS, which
  is the recommended order.
* **H9 — reset on a size change.** CLEARED. The feature is released and rebuilt
  whenever `w/h/outW/outH` move (`dlaa.cpp:239-243`), and the pass's own
  textures likewise (`temporal_pass.cpp:1230-1252`), so a resolution change
  cannot carry stale history. The problem is the opposite one, F1.
* **H10 — sRGB versus linear.** CLEARED. `viewFormatOf`
  (`temporal_pass.cpp:381-406`) refuses `_SRGB`-typed sources outright, and the
  logs measure the submitted format as `R8G8B8A8_TYPELESS (27)` on every
  flight. The frame at the door is tonemapped, HUD-composited, 8-bit —
  LDR is the right mode and `IsHDR` is correctly not set. The colour copy
  preserves the bits. The one wrinkle is F9's format change on the output.
* **H11 — the own pass's low-passes leaking into the trained path.** CLEARED.
  The colour NGX sees is a raw `CopySubresourceRegion` of the game's region
  (`temporal_pass.cpp:1270-1274`) — no Gaussian, no clip, no blend. The rest
  snap lives only in `fetchHistoryT` (`:189-193`) and the `mv` entry does not
  call it. The 3x3 nearest-depth dilation is applied to the MV's reprojection
  (`:225-230`) but the depth handed to NGX is the undilated `zraw`
  (`:222`, `:247`) — which is right on both counts.
* **The own pass's softness is by design, and it is large.** The Gaussian on
  the current sample, `exp(-2.29 d^2)` (`temporal_pass.cpp:285`), is
  sigma ≈ 0.467 px; its modulation at the sample grid's Nyquist is
  `exp(-2 π² σ² f²)` with f = 0.5 = **0.34**. A one-pixel stroke keeps about a
  third of its contrast in the current sample before the blend even starts, and
  the cubic history resample takes more out at each of the ~10 frames a 0.90
  blend averages over. This is UE4's trade and the code says so; it is not a
  bug. The levers already exist and are live:
  `advanced.temporal_aa_current = raw` removes the Gaussian,
  `advanced.temporal_aa_history_sharp = 0.75` sharpens the resample, and a
  lower `fix.temporal_aa_blend` shortens the average. If the player's priority
  is text, that trio is the A/B to fly — separately from anything above.

---

## Structural themes

* **One flag is doing two jobs.** `EyeState::haveHistory` means "the pass's own
  ping-pong holds a usable frame". The trained path borrows it for two
  unrelated questions — "should NVIDIA start afresh?" (F1) and "is `tanPrev`
  trustworthy?" (F7) — and gets the wrong answer to both, permanently. Give the
  trained path its own continuity flag and the two bugs disappear together.
* **The instruments measure the own pass, not the trained one.** The
  registration statistics, the price query, the "first treated frame" line and
  every smoke test observe `main`. Nothing observes the `mv` texture, the
  jitter that reached NGX, or the reset. Fifteen builds of field evidence say
  nothing at all about the path the player is actually running
  (`temporal_aa = dlss` in the live `edvr.ini`).
* **A shared mapping cannot cross-check itself.** `fetchHistoryT` and `mv`
  transcribe the same lines, which is good discipline and is exactly why the
  12.8%-versus-21% verdict cannot vouch for the MV texture: a sign error in the
  mapping would move both instruments together. The MV texture needs a test
  with an externally known answer, not a comparison against its own twin.
* **Two conventions meet at the door and neither is written down at the
  boundary.** EDVR's `(jx, jy)` is derived and stated in
  `temporal_math.h:26-28`. NVIDIA's is stated only as "pixel space". The
  handoff at `dlaa.cpp:321-322` is a bare assignment with no comment naming the
  measurement that justifies it — the one place in this branch where a number
  crosses into another vendor's contract unverified.
* **A silent failure that prints its own zeros.** F2 fails on every call and
  the log dutifully reports `0x0..0x0` as if the runtime had answered. A query
  whose failure is indistinguishable from its success is worse than no query.

---

## Phased fix order

**Phase 1 — fix before the next flight; no measurement needed.**

1. F1, the reset. One flag, and it is the largest single suspect.
2. F2, the capability parameters for `NGX_DLSS_GET_OPTIMAL_SETTINGS`, plus a
   log line when it fails and mode selection from the real optimal size.
3. F7, `tanPrev` on the trained path (falls out of F1 if done deliberately).
4. F11, `InFrameTimeDeltaInMsec` from the frame rate the totals line already
   measures.
5. F12, the trained path's log lines: format, MV/depth formats, this frame's
   jitter, the reset count.

**Phase 2 — instrument, then fly to decide.**

6. F4, the projection-read-order counters. One line; it either clears the
   jitter's phase or names it as the bug.
7. F8, head translation in millimetres on the registration line, and the
   depth-versus-rotation comparison bucketed by it.
8. The flight: DLAA at HMD Quality 1.0 against DLSS at 0.67 with the same
   output size, back to back, with F1 fixed — which separates F3 (the interface
   surfaces) from everything the pass controls.

**Phase 3 — the desk tests, then the remaining signs.**

9. F5, the jitter-sign readback tests (both axes), and the MV readback test
   below. Hard-code the answer with a comment naming the measurement.
10. F9, F10, F13: the latent format, region-origin and per-eye-tangent
    assumptions. None of them is live on this rig; fix them when the code near
    them is open anyway.

**Not now.** F6 (the ship's motion) is a feature, not a fix, and belongs with
`advanced.temporal_aa_motion = camera` and the world/cockpit split.

---

## The desk tests to add

The existing tests cannot catch any confirmed finding. `tools/smoke/smoke.cpp`
lines 646-726 and 778-808 feed the pass SOLID COLOURS: `checkResolved` compares
one pixel against the colour that went in. Every reprojection, every jitter and
every motion vector produces the same answer on a flat field, so a reversed
sign, an included jitter and a reset-every-frame all pass. Three tests fix that.

**T1 — the reset is not asserted every frame (catches F1).**
Add a `resets` out-parameter to `dlaaTotals` counting evaluations issued with
`InReset != 0`. In the harness: one call with `flags = 1u | 2u`, then eight with
`flags = 2u`, same 400x304 source, same eye. Expect `evaluations == 9` and
`resets == 1`. On a machine without the runtime the test reports "skipped, no
NGX" the way the existing dlaa block already does.

**T2 — the motion vectors have the right magnitude and sign (catches an MV
error, and is the fixture F5's test builds on).**
Export the last-written MV texture for an eye
(`edvrTemporalMvTexture(int eye)` returning the `ID3D11Texture2D*`), copy it to
staging with the harness's existing readback helper, and check the pixel at
integer coordinates **(200, 152)** — the shader's `p` for that thread, half a
pixel off the frustum's true centre, which is where the numbers below were
computed. With `tan = {-1, 1, -1, 1}`, a 400x304 region, no depth and a delta
that is a pure rotation, the arithmetic is closed-form:

* delta = a rotation of +1° about +Y (row-major `{cos,0,sin; 0,1,0; -sin,0,cos}`):
  centre pixel MV = **(-3.491, 0.000)** px, tolerance 0.02.
  (`MV.x = -w/2 * tan(1°) = -200 * 0.017455`.)
* delta = a rotation of +1° about +X (`{1,0,0; 0,cos,-sin; 0,sin,cos}`):
  centre pixel MV = **(0.000, -2.653)** px, tolerance 0.02.
  (`MV.y = -h/2 * tan(1°) = -152 * 0.017455`. Negative is the whole point: a
  head that pitched up finds the current centre pixel HIGHER in the previous
  frame, i.e. at a smaller row.)
* identity delta: MV = **(0, 0)** everywhere.

Check two more pixels off-centre — say (100, 76) and (300, 228) — against the
same formula walked by hand, so a transposed row cannot pass. `tools/temporal_test`
already pins `temporalReproject` on the CPU; this is the same arithmetic pinned
on the GPU, which is the half that ships.

**T3 — the jitter's sign, per axis (settles F5).**
The sub-pixel line test described under F5: a one-pixel horizontal line drawn
with its energy split between two rows by the frame's `jy`, sixteen frames
through the trained path with no motion, then a readback of column 200. Correct
sign: the peak returns to row 152 within a few 1/255ths of the drawn peak and
the neighbours are near black. Inverted sign: two rows at roughly half the
peak. Repeat with a vertical line and `jx` for the x axis. Run it only when NGX
is available and say "skipped" otherwise, like the existing dlaa block.

A fourth, cheaper one is worth having even though no finding needs it: extend
`tools/temporal_test/temporal_test.cpp` with the `temporalJitterToTangents`
round trip — shift the tangents by `(dx, dy)`, project a fixed direction
through the shifted frustum with `temporalDirToPixel`, and assert the pixel
moved by exactly `(+jx, +jy)`. That pins EDVR's own convention in a test
instead of in a comment, which is what F5's fix will be compared against.
