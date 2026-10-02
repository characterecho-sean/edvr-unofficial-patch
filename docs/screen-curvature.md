# The on-foot screen: distance and curvature

## Status

- **State: retired 2026-09-29 (code removed, 50cc79ae).** The shader
  input-signature reader (`shader_sig.cpp`) that settled the z question below
  had no caller left and is deleted. The finding stands. Recover the code from
  `50cc79ae^` (`src/d3d11/shader_sig.cpp`).

Elite renders everything on foot (and in HMD Cinema Mode) to a big flat
virtual screen hanging in space. Two EDVR settings shape that screen into
something more comfortable to sit in front of: `panel_distance` moves it,
and `panel_curvature` bends it toward you, the way Virtual Desktop curves
its virtual display. This is the write-up of how both work, what was
measured to build them, and how to tune them. Both are field-verified
(2026-08-23, Quest 3 and Pimax rigs).

**Suggested starting point on foot: `panel_curvature = 0.3` with
`panel_distance = 0.7`.** A closer screen fills more of your view; the curve
keeps its edges at a comfortable distance instead of letting them fall away.
Both settings are live — save the file and the screen follows within a
second — so tune by eye from there.

---

## What the settings do

- **`panel_distance`** (default 1.0 — the game's own placement): scales how
  far away the screen sits. 0.5 is noticeably close, 2.0 far. Implemented by
  scaling one float in the panel's own placement constants for exactly the
  panel's draw; nothing else sees the change.
- **`panel_curvature`** (default 0 — flat, off): the fraction of a full
  circle the screen wraps around you. 0.1 is a gentle theatre curve, 0.3 a
  comfortable wrap, 0.25 a quarter circle. The bend preserves the screen's
  width along the arc (a curved cinema screen, not a stretched one), so the
  edges come nearer rather than the middle moving away. The image itself is
  untouched — the same texels, on bent geometry.

**With the VR on-foot world route** (`experimental.temporal_aa_on_foot_world =
auto`, its default since 2026-10-01; design-flat-temporal-aa-2026-09-23.md,
section 82, "The curved route") the curve stays on. The route hands each eye its own resolved copy of
the screen through the UI layer, and for a curved screen the layer draws that
copy through the very same bent strip the game's own draw uses, by the same code,
so the bend and the placement are identical with the route on or off. The route's
5 s log line names the curve in use (`curve=`). A bent screen's edges come
nearer than its middle, so on a near screen they show the picture enlarged more
than the middle does; if they look softer than the middle at
`vscreen_res_width = auto`, type a width.

**The intro movie and the splash screen follow the same setting** (design:
intro-video.md, 2026-10-01). EDVR places the movie's quad itself
(`fix.intro_video = screen`), so above 0 it is drawn as the same bent strip,
with the depth axis added to the placement it builds. The splash is the game's
own world-space panel: EDVR copies its placement constants (again every 60
frames while they read as anything but a world-space panel), reads from them the
panel's half-width, which way its depth runs and which way its +x runs on the
screen, and then draws the same strip through the game's own constants. The last
one matters for the picture: the intro composite's +x runs to the viewer's left
(the game's own quad compensates in its vertex data), so the strip's u runs
against x there -- with x, as it always did, for the on-foot screen -- and it is
read from each placement, never assumed (the first flight of the curved intro
came out mirrored because it was). Both draw the strip without back-face
culling, because nobody has recorded that composite's index order. At curvature
0 none of this runs and a rig holds the movie's and the splash's draws byte for
byte. What has no depth to bend stays flat: the movie in `stock` and `head`
modes (head-locked screen space) and any composite whose constants do not read
as a world-space panel; the log says which and why.

Under `[advanced]`: `panel_curvature_segments` (how finely the screen is
tessellated; 64 is past what the eye can see, and below ~8 the bend visibly
facets), `panel_curvature_sign` (which way the bend goes — the escape hatch
if a game update flips the panel transform's handedness), and
`panel_curvature_z_gain` (the bend's depth scale — read automatically from
the game, overridable if an update moves it).

## How the screen is actually drawn, measured

Everything below was measured through the `d3d11.dll` proxy — a draw
census, a one-shot capture of the composite's vertex and index buffers, a
DXBC input-signature reader, and finally the composite's own vertex shader
dumped and disassembled (`docs/shaders/composite-vs.asm`). No game file or game code
is modified.

Every frame on foot and in HMD Cinema Mode, the game composites the
panel-sized render (the screen's image) into each eye with one small
indexed draw:

- **The geometry is a canonical unit quad**: four 20-byte vertices —
  `float3` position at the corners (±1, ±1, 0) and `float2` UV — plus a
  dedicated six-index buffer (`0,3,1, 0,2,3` — two clockwise triangles,
  and back-face culling is on, so winding matters).
- **Placement lives in constants, not vertices.** A 208-byte constant
  block sizes, places and projects the quad per eye — which is why one
  scaled float moves the whole screen (`panel_distance`), and why both
  eyes share one vertex buffer and still come out in stereo.
- **The vertex shader scales x and y by a SIZE input, and passes z through
  raw.** From the disassembly:

      mul r0.xy, v0.xyxx, v2.xyxx      POSITION.xy * SIZE.xy
      mov r0.z,  v0.z                  POSITION.z, unscaled

  then an honest projective transform to world and clip. That one
  asymmetry is the whole story of the feature's hardest bug (below).

## How the bend works

At `panel_curvature > 0`, EDVR substitutes the quad for exactly that draw:
a strip of N columns (two rows of N+1 vertices) whose positions trace a
circular arc in the panel's local space —

    theta = pi * c * x                 x in -1..1
    x'    = sin(theta) / (pi * c)      arc length preserved
    z'    = -gain * (1 - cos(theta)) / (pi * c)    toward the viewer

— with the original flat x driving each column's UV, so the bend moves
where a column *is*, never which texel it shows. The game's own shaders,
constants and pipeline state then draw the bent strip exactly as they drew
the flat quad: per eye, with the game's own projection, in real stereo. At
curvature 0 with one column the substituted strip is the game's own quad to
the byte — the identity test that separates substitution faults from
geometry faults, and the first thing that was field-verified.

The **gain** is the part that cost the debugging arc. The shader scales x
and y into the panel's model size but consumes z in raw local units — so a
geometrically correct bend displaced the edges by a percent or two of the
panel's width: real, measured, and invisible in stereo, while the
arc-length narrowing in x (riding the scaled basis) showed at full
strength. The symptom was exactly "the image squishes horizontally and
nothing bends." The fix reads the panel's SIZE from the game's own buffer
and multiplies the bend's z by it, putting depth in the same units as the
width it curves. `panel_curvature_z_gain` overrides the reading if a game
update ever moves it.

Safety shape, same as every EDVR fix: the substitution saves exactly the
input-assembler state it changes and restores it after the draw, with the
saved state reachable from the fault path; any fault stands the feature
down for the session on the spot and the game's own quad draws again. A
build failure means a flat screen, never a missing one.

## The debugging history, kept for the method

Five findings, each of which cost or saved a flight:

1. **One column cannot bend.** The curve lives in the interior vertex
   columns; the two edges get the same depth whatever the curvature
   (cosine is even). A 1-column strip at curvature 0.3 just moves away and
   narrows — which reads as "it bends the wrong way."
2. **+z in the panel's local space is away from the viewer** — measured
   with a flat z displacement, which doubled as the sign test.
3. **A mono screenshot cannot show a cylindrical bend.** A curved screen
   viewed head-on looks like a flat, narrower one — that is what curved
   monitors are for. Two flights were misread from captures; curvature is
   judged in the headset only.
4. **"Reads z" and "projects z at the right scale" are different facts.**
   The DXBC input signature (`POSITION0 used=xyz`) proved z was consumed;
   a flat z-probe proved it reached the screen; and both were still
   compatible with an invisible bend, because the units were wrong. The
   disassembly settled it in one read — the project's most-paid-for
   lesson, paid again: the shader's actual text beats any black-box probe.
5. **The narrowing is the feature working.** Arc-length preservation gives
   `sin(pi*c)/(pi*c)` of the flat width — 0.98 at c = 0.1, 0.86 at c = 0.3.
   The width is on the curve; the chord is shorter.

## Tuning notes

- Distance and curvature compose: nearer screens want more curve. The 0.7 /
  0.3 pairing above is the tested on-foot starting point; in HMD Cinema
  Mode in a ship, taste varies more — start from the same pair.
- The bend costs nothing measurable: the substituted strip is 130 vertices
  where the quad was 4, in a frame that draws millions.
- If a game update changes the composite, the recognition simply stops
  matching and the screen draws stock — the log says so. The escape
  hatches (`panel_curvature_sign`, `panel_curvature_z_gain`,
  `panel_distance_index`) exist for the subtler case where it still
  matches but a convention flipped.
