# Panel curvature: why the bend reads as a squish — review handoff (2026-08-23)

For the agent implementing `panel_curvature`. A review of the code on main
(`7040290`..`7f044fb`) against every field result to date, with the leading
defect named, the one measurement that confirms it, and the fix paths.

**Short version: the substitution machinery, the strip geometry, the winding
and the x math are all sound — the field's horizontal squish is the arc-length
x term landing exactly as designed. Vertex z DOES reach the projection
(flight 3's flat z-probe visibly moved the panel, which rules out the
"shader ignores z" theory the probe was built to test). The leading defect is
a SCALE mismatch: the panel transform's z basis appears to be much smaller
than its x/y basis, so a bend that is geometrically correct in local units
displaces the edges by millimetres in the world — invisible in stereo —
while the x narrowing (which rides the healthy x basis) shows at full
strength. The user's report is exactly that split: "increasing the value
only seems to squish the image horizontally."**

## The evidence, reconciled

1. **The squish is the x math working.** Width narrows by `sin(pi*c)/(pi*c)`
   — measured 0.637 at c = 0.5, the arc-length prediction to three digits.
   The strip is drawn and its x displacements land.
2. **z reaches the output.** Flight 3: `panel_curvature_z_test` at +0.5 and
   +0.9 moved the panel in depth (further away at +0.9, confirming +z away
   and `kTowardViewer = -1`). Any theory that says the shader drops z is
   dead — including the depth-only-output theory this reviewer first
   reached for before flight 3's result was consulted.
3. **The signature agrees**: `POSITION0 used=xyz` (session 09:47).
4. **Yet the bend is invisible in the headset** even at high curvature,
   where the edge displacement is 0.4–0.6 local units — over half the
   panel's half-width. If those units projected at the same scale as x,
   the edges would come dramatically nearer and stereo could not miss it.

The only reading that satisfies 1, 2 AND 4 together: z projects, but
through a much smaller basis than x. A flat quad's placement transform has
no geometric reason to keep its z column at the same scale as x/y — any
nonzero column makes a probe "move the panel", while being useless for a
bend that needs world-metric displacement.

## The one measurement that settles it

Ask the pilot (or re-fly the probe): **how FAR did the screen move at
z_test = +0.9?** The panel's half-width is 1.0 in the same local units.

- Moved a large fraction of its own size → the z basis is healthy, this
  theory is wrong, and the shader's own math must be read before anything
  else is tried (dump path below).
- Moved subtly — centimetres against a metres-wide screen → scale mismatch
  confirmed; proceed directly to the fix.

Better than asking: **measure the basis directly.** The composite's
208-byte constant buffer is already shadowed by the panel machinery
(`panel_distance` edits its float 47). Log floats 0..51 once (the
`panel_quad_dump` pattern) and read the transform's columns: the x, y and z
basis magnitudes are sitting in there, and their ratio is the gain the fix
needs. No flight-by-feel required.

## Fix paths, in order of preference

**A. Gain the bend's z by the measured basis ratio.** One multiply in
`bend()`: `z' *= |x-basis| / |z-basis|`, with the ratio read live from the
shadowed CB (or, first pass, a `panel_curvature_z_gain` advanced key swept
in the field to confirm the mechanism before wiring the automatic ratio).
Smallest change; keeps the game's shader; the whole existing strip
machinery stays exactly as built. Risk: if the z basis is not merely small
but skewed (not parallel to the panel normal), a scalar gain bends in a
slightly wrong direction — the CB read will say.

**B. The sun-glare playbook: replace the composite's vertex shader.** Full
control: build the position in view space from the panel's own constants,
displace along the true plane normal in world metric, project with a real
per-vertex w. The machinery (runtime D3DCompile with the ELEVEN-parameter
signature, desk-compile-first workflow, fail-soft to stock, fault budget)
all exists in `sunglare_fix.cpp` / `sunglare_vs.h`. Needed anyway if A's CB
read shows a degenerate or skewed z column. First step either way:
`advanced.glare_shader_dump = 1` (startup-read, one restart), bring the
screen up once, and the composite's VS lands in `edvr_logs\shaders` — it
will be the only one whose input signature carries a SIZE element (131
shaders from the 2026-08-22 session were checked; none has one, because
the panel never came up that session). Disassemble with the scratchpad
`disasm.py` pattern and read what the placement math actually does with
`pos.z` and with the constants — ground truth beats every black-box probe,
which is this project's most-paid-for lesson.

## A loose end worth one check: SIZE0

The composite's VS consumes a third input, `SIZE0 r2 used=xy`, which cannot
fit the 20-byte stride — it rides another IA slot. The game's buffer there
covers 4 vertices; the substituted strip indexes up to 130. If that slot is
per-VERTEX stepped, interior vertices read past the buffer's end (D3D
defines this as zeros) and anything SIZE scales would collapse for them —
no such artifact is visible, so it is probably per-instance or constant,
but the input layout's step mode has never been read. Confirm it while in
the shader dump (the layout is created alongside), or note that a visible
per-column artifact under path A would implicate exactly this.

## What is NOT the defect — reviewed clean, do not re-spend here

- The strip geometry: arc-length bend, interior-column reasoning,
  UV-from-original-x, measured winding (0,3,1 / 0,2,3 against clockwise
  front + back-face culling), the byte-identical `segments=1 c=0` identity.
- The substitution: save/substitute/draw-through-original/restore, saved IA
  state at module scope for the fault path, first-fault permanent
  stand-down. Sound, and the right paranoia.
- The sign machinery: measured, correct, moot until z displaces visibly.
- Perception traps already documented in the workstream notes: a mono
  screenshot cannot show a cylindrical bend, and one column cannot bend
  (the curve lives in interior vertex columns). Both cost a flight each;
  neither is in play now that the report comes from the headset at 64
  columns.

## House rules that have bitten before

`build.bat` has an explicit source list (a new .cpp must be added).
PowerShell 5.1: no `&&`, quote-free commit messages, gate on
`$LASTEXITCODE`. Desk-compile any HLSL before shipping (`compile_variants.py`
pattern) — the game is never the compiler's first audience. Never overwrite
the game-dir `edvr.ini`; edit in place. Both DLLs install as a pair;
install is blocked while the game runs. Judge curvature only in the
headset.
