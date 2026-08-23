# Panel curvature: why the bend renders flat — review handoff (2026-08-23)

For the agent implementing `panel_curvature`. A review of the code on main
(`7040290`..`7f044fb`) against the field results, with the defect named and
the path forward. Short version: **the substitution machinery is sound and
the geometry is right; the defect is that this composite's vertex z never
reaches the projection — it is consumed as output depth only, so no
geometry-only substitution can ever bend this screen.** The fix is the
sun-glare playbook: replace the composite's vertex shader for exactly this
draw.

## What the field says

Three facts, and they only fit together one way:

1. **The bend renders as a horizontal squish.** At curvature > 0 the image
   narrows by exactly the chord/arc factor the x math predicts — so the
   substituted strip IS being drawn, with our x displacements landing —
   and shows zero depth: no parallax, no stereo change, no edge-nearing
   (user report, 2026-08-23: "increasing the value only seems to squish
   the image horizontally").
2. **The flat z-probe moved nothing.** `panel_curvature_z_test` at +0.5 and
   +0.9 — half the panel's own half-width — produced no visible
   displacement (sessions 09:36 and 09:41; the pivot to building
   `shader_sig` immediately after is this result's paper trail).
3. **The shader reads z.** The DXBC input signature (session 09:47):
   `POSITION0 r0 has=xyz used=xyz; TEXCOORD0 r1 has=xy used=xy;
   SIZE0 r2 has=xy used=xy`.

Fact 3 looks like it contradicts fact 2. It does not: *reading* z and
*projecting* z are different things. `used=xyz` says the shader dereferences
the component; it says nothing about what it feeds. The construction that
satisfies all three facts at once is the standard screen-space composite:

    out.xy = f(pos.xy, SIZE, constants)     -- placement, no perspective
    out.z  = g(pos.z)                       -- DEPTH for the depth test
    out.w  = 1                              -- no divide

Under that shader, our bent x lands (fact 1's squish), our z changes only
what the panel occludes — invisible against empty room depth (fact 2) — and
the signature honestly reports z as read (fact 3). It is the same disease
the sun-glare shader had (`docs/sun-glare.md`): a screen-plane construction
with `w = 1`, correct on a monitor, unbendable in space.

Two corroborating details:

- **`SIZE0` is a third input that cannot fit the 20-byte stride** (float3 +
  float2 fills it). It rides another vertex stream, slot 1+ — untouched by
  the substitution, which is fine — and "size" as a per-vertex semantic is
  the idiom of a scaled screen-space placement, not of a world mesh.
- **`panel_distance` works by editing a constant** (index 47 of the panel's
  constant data), not a vertex: the panel's placement lives in constants.
  The vertices are a unit square the shader scales into place — exactly the
  shape of construction that has no use for a vertex z in x/y.

## What is NOT the defect

Reviewed and clean — do not re-spend time here:

- The strip geometry: arc-length-preserving bend, interior-column reasoning,
  UV-from-original-x, the measured winding (0,3,1 / 0,2,3 against clockwise
  front faces, back-face culling), the byte-identical `segments=1 c=0`
  identity case. All correct.
- The substitution: save/substitute/draw-through-original/restore with the
  saved IA state at module scope so the fault path can restore, first-fault
  permanent stand-down. Sound, and the right paranoia.
- The sign machinery (`kTowardViewer`, `panel_curvature_sign`): moot until z
  projects at all, but not wrong.
- The x math: proven working in the field by the squish itself.

## The fix: replace the composite's vertex shader (the sun-glare playbook)

Geometry alone cannot do it; the draw's own shader must. Everything needed
already exists in-tree from the sun-glare arc:

1. **Dump the shader.** `advanced.glare_shader_dump = 1` (startup-read, one
   restart), then bring up the on-foot screen or HMD Cinema Mode once. The
   composite's VS lands in `edvr_logs\shaders\vs_<hash>.dxbc`. It is
   identifiable by its input signature (the only VS with a SIZE element —
   131 shaders from the 2026-08-22 dump session were checked and none has
   one, because the panel never came up that session). Disassemble with the
   scratchpad `disasm.py` pattern (`D3DDisassemble` via ctypes).
2. **Read the placement math.** Expect: position built from `pos.xy`, SIZE
   and constants; find where the panel's centre/basis/distance come from
   (the constant block `panel_distance` already edits — the placement data
   is measured and shadowed). Confirm what `pos.z` actually feeds.
3. **Write the replacement** against the disassembly — same inputs (declare
   SIZE0 even if unused), same outputs, same constants — but build the
   position in VIEW space: the panel's plane from its own constants, the
   vertex xy on it, the bend's z along the plane normal, projected with a
   real per-vertex w. The bend can then move INTO the shader (compute from
   x, feed curvature via a b2-style constant buffer, exactly like the glare
   fix's CBT) — which retires the strip's x-displacement too and lets the
   game's own flat quad flow through with just more columns needed for
   nothing: keep the strip substitution anyway for the interior vertices
   (a quad has none), but its x can stay UNBENT with the shader doing all
   displacement, which keeps UVs trivially right.
4. **Compile-and-swap machinery**: copy the sun-glare pattern wholesale —
   runtime `D3DCompile` through `d3dcompiler_47` with the FULL
   eleven-parameter signature (see `sunglare_fix.cpp`; the ten-parameter
   typedef crashed the game once), desk-compile with the scratchpad
   `compile_variants.py` pattern BEFORE any install, fail-soft to stock,
   fault-budget around the build, swap in the same begin/end bracket the
   strip substitution already owns.

Notes for the implementer:

- Whether the composite draws once per eye with per-eye constants (the
  glare train does) decides nothing here — the shader path handles either,
  since the constants are bound per draw.
- The one thing the replacement must NOT change at curvature 0: output
  byte-equality of placement with the original is unprovable from outside,
  so keep the strip's `c = 0` identity test philosophy — a `curvature = 0`
  swap must be visually indistinguishable, and the first field pass should
  verify exactly that before any bend is attempted.
- The depth the original writes (`g(pos.z)`) is load-bearing for occlusion
  (cockpit geometry could overlap the panel edge): reproduce it, don't
  invent a new one.
- House rules that bit before: `build.bat` has an explicit source list (a
  new .cpp must be added there); PowerShell 5.1 (no `&&`, quote-free commit
  messages, gate on `$LASTEXITCODE`); never overwrite the game-dir
  `edvr.ini`; both DLLs install as a pair; install blocked while the game
  runs.

## Cheapest confirmation before writing any of it

One restart pass: `glare_shader_dump = 1`, bring the screen up, restart-off.
The disassembly then *proves* the depth-only construction in one read (look
at what feeds `o*.z` vs `o*.xy` / whether any output is a real `w`), and the
replacement gets written against ground truth instead of a strong inference
— the sun-glare arc's own lesson, twice paid: the shader's actual text
beats any amount of black-box probing.
