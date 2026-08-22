# Sun-glare world shader: adversarial review handoff (2026-08-22)

Standalone brief for whoever implements the next step of the sun-glare
world-anchor arc. Written after an adversarial review of the deduction
chain; read this before trusting any conclusion in the commit messages
between `b712d82` and `640f2d5`.

## Where the arc stands

The replacement vertex shader (`src/d3d11/sunglare_vs.h`, selected by
`fix.sun_glare_world`) draws the glare train as world-anchored
billboards. Field-verified: correct and world-stable while the pilot's
head is within ~45 degrees of ship forward; vanishes at a clean line
beyond. Matched draws continue at ~180/s through the vanish, so the
game is not culling the call — the death is in what the draw consumes.

Three successive theories about that death are now all **unproven**:

1. **Camera clamp** (the glare CB rows freeze at ±45° head-look) —
   retired when telemetry showed `align 1.0000` and `cf == tf` through
   a full sweep… but see F3 below: that reading rides an unverified
   assumption and may be a tautology.
2. **Honest rows, stale element position** (`v1` computed CPU-side in
   a head-look frame) — plausible, never directly observed. The
   per-instance streams have never been read even once.
3. **Origin = sun** (rebuild elements on the camera→origin ray) —
   falsified in the field: the solved origin is a floating anchor that
   re-bases near the ship (~every 25 s) and recedes at ~190 u/s, which
   is most simply the ship's own cruise speed through a re-based world
   (review finding F5). The rebuild is now held dark
   (`tValid.y = 0`, `sunglare_fix.cpp`).

## The review's decisive findings

**F1 (BROKEN) — the E4 paradox.** With the rebuild live, `epos =
cam·(1 − L/d)`, and projecting through the same rows the solve used
gives `clip(epos) = (L/d)·(w4,w5,w7)` — *pixel-identical to the
projected world origin, for any L and any head pose*. Under the
chain's own premises (shadow = the draw's b0; rows honest and
head-tracked; origin world-fixed) the rebuilt disc is world-stable at
the anchor's bearing. The pilot saw it **follow his head**. That is a
theorem-level contradiction: at least one premise is false. Candidates,
with the reviewer's weights: the shadow is not the draw's b0 (~50%),
the visual report is misdescribed (~25%), the instance stream differs
from assumption in an unimagined way (~25%).

**F2 (BROKEN) — "No change" proved nothing.** The composed-true-rows
build recalibrated `P = shRows × V⁻¹` on every aligned draw and
composed `b2 = P × V` the same draw — an algebraic identity whenever
the align gate is open, and the field log shows it open at every
telemetry second. The session before that ran with the pose-convention
bug (`align 0.99, P--`), so substitution never engaged there either.
**No field pass has ever exercised rows that diverge from cb0.** The
clamp theory was retired without ever being tested.

**F3 (SUSPECT) — buffer identity is unproven.** The 208-byte shadow's
target comes from `bindingGet(BindSlot::VsCb0)`
(`billboard_fix.cpp:152`), a mirror that only sees plain
`VSSetConstantBuffers`. Gaps: `VSSetConstantBuffers1` (D3D11.1 offset
binds) is not hooked; null unbinds don't clear the record;
`UpdateSubresource`/`CopyResource` writes are invisible to the Map tee.
If the game binds the real glare block through any of those, our
mirror holds a stale pointer — plausibly the scene camera's block,
which the game maps every frame, keeping the shadow perfectly fresh
and perfectly wrong. `cf == tf` then compares the scene camera with
itself. Nothing in any log to date can distinguish this.

**F4 (SUSPECT) — every "origin is sunward" reading is sign-blind.**
The eccentricity aggregate uses `fabsf` and min/max across both eyes;
it cannot tell the sun from its antipode, and the anchor lies along
the recent travel axis, which every glare test flies sunward — the
historical corroboration was selection bias.

**F7 (SUSPECT) — the death mechanism is unconstrained.** A clean
disappearance line is equally produced by stale position (`v1`), a
zeroed size chain (`p1`/`p2`), or a CPU-side alpha fade (`t4.x`,
passed straight through by both the original and replacement shaders).
The shader-side gates were checked and cannot manufacture the line
from honest inputs. The line lives in the inputs; which input is
unknown because the streams have never been observed.

**Verified and standing:** the Cramer camera solve (correct, convention
matches the original shader's `dp4` usage); the pose matrix at scene
offset 932 and its convention; TAA jitter cannot explain the drift;
draws continue through the vanish; b2 packing C++↔HLSL agrees; the
current build's rebuild is truly inert.

**Lower-severity:** F8 latent target-thrash if `fix.billboard=steady`
runs beside world mode (one shared `g_target`); F9 the 208-byte
scene-CB nomination in `vscreen.cpp` is dead code (its threshold can
never fire since 6957ebf); F10 a shared fault budget can freeze the
shadow and the pose feed together with no staleness flag — mitigated
now by the `age=` field.

## The discriminating build (installed, commit pending as of writing)

All instrumentation, no behavior change; rebuild stays dark. Per
telemetry second while the world shader is active:

- **`glare b0 identity:`** runtime `VSGetConstantBuffers(0)` pointer
  vs `billboardTarget()` (the mirror), plus the buffer's ByteWidth and
  Usage, verdict `MATCH`/`MISMATCH`. **This single line decides F3.**
  MISMATCH ⇒ every constants-based conclusion re-opens, clamp theory
  restored; sustained MATCH with ByteWidth 208 and Usage DYNAMIC ⇒
  identity holds and F1 points at the instance stream instead.
- **`org=(x y)`** on the main line: the *signed* projected origin
  `(sh19/sh31, sh23/sh31)`. Under head sweeps: tracking vs pinned is
  now measured per second, replacing the sign-blind ecc aggregate.
- **`age=N`**: ms since the shadow content last updated. Stale ⇒
  identity disproven on its own (F10's failure mode also covered).
- **`glare stream slot …`**: first 384 bytes of every bound IA vertex
  buffer, every 2 s, budget 30 — the instance-stream discovery dump.
  Decode offline: find `v1` (POSITION0, expect ~50-magnitude triplets),
  the size chain, and the alpha; watch which attribute dies at the
  disappearance line, and whether `v1` rotates under pure head motion
  inside the clamp (world-frame v1 is static there; head-look-frame v1
  rotates — the deepest single discriminator).
- Camera-block census (`glare camblock`): now fires on an ~11° head
  turn since the last shot (budget 8, min 2 s apart) instead of a
  timer that burned shots while the headset sat on the desk.

## The pilot script for the pass

1. Park throttle-zero at a star, 60 s still: `d` must hold constant
   (confirms F5's ship-speed reading; a drift while parked re-opens it).
2. Head still 10 s, then small head turns *inside* 45°, pausing
   between: census shots + stream dumps under pure head rotation
   (the v1-frame test).
3. Two slow sweeps *past* 45°: bracket the death in the stream dumps,
   watch `org=` tracking-vs-pinned and the identity line through it.
4. 30 s of ship yaw with head still: does align survive? (Tests
   whether the offset-932 pose is world- or ship-framed — currently
   unknown.)
5. Throttle up toward the sun 30 s: the `d` sawtooth should resume at
   the HUD's speed reading.

## Decision tree on the results

- **MISMATCH on identity** → hook/observe the real b0 path
  (`VSSetConstantBuffers1`), re-shadow the true buffer, re-run the
  align question against it. Expect the clamp theory to return; the
  fix is then true-row substitution into b2 — machinery that already
  exists and is verified inert-safe.
- **MATCH + `org` pins under head sweep** → rows honest but the
  translation column is head-framed: the fix is recomposing the w
  column per draw from the solved cam (already have) and a sun
  position from the census/streams.
- **MATCH + `org` tracks + a stream attribute dies at the line** →
  theory 2 confirmed with the attribute named; fix = substitute that
  attribute (position from latched world sun direction + solved cam;
  or size/alpha floor) in the replacement shader via b2.
- **`d` drifts while parked** → F5 wrong, anchor kinematics unknown —
  stop and re-model before building anything.

## Standing cautions

- `sun_glare_world=3` (ALLWORLD) forces every instance down the world
  path; any epos-style rebuild collapses all 15 instances onto one ray
  — flare-ghost sliders included. Re-light experiments under variant 1.
- The pilot flies passes at real cost (headset on, launch, fly to a
  star: ~20 min each). Never ship an instrument whose firing conditions
  haven't been walked through against the actual session shape (the
  first census burned its budget on a desk-bound headset).
- PowerShell 5.1 rules for any agent working this repo: no `&&`, no
  quotes inside git-commit here-strings, gate on `$LASTEXITCODE`.
- Desk-compile all four shader variants before any install
  (`compile_variants.py` pattern); the game is never the compiler's
  first audience.
