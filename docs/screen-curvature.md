# Curving the on-foot screen: what it would take

*A design exploration, written from reading the code. Claims about
EDVR cite the source; claims about the game's composite draw are labelled
believed or measured, and the ones that can only be settled in a live
session are collected in one list near the end, labelled as such.*

*The bend is not implemented and has never been run against the game.
Phase 0 — the measuring — is under way, and the section at the foot of
this file ("Phase 0, in progress") records what it has found so far,
including one premise above that turned out to be wrong. Read that
section before acting on this one.*

The ask, from users: Virtual Desktop shows its virtual screen as an
optionally curved surface — a cylindrical bend toward the viewer, so the
edges of a large screen sit at roughly the same distance as its centre —
and the on-foot / HMD Cinema Mode screen should offer the same. It is a
comfort feature with the same shape as `panel_distance`: at 2.0 the flat
panel's edges are noticeably farther away than its middle, which is
exactly when a curve is wanted.

The short version of the whole document:

- **The mechanism EDVR already owns is the right one.** The panel distance
  fix stands inside the exact draw that would need to change: the panel
  composite, recognised per draw, with the game's transform buffer
  shadowed and a substitute bound for one draw
  (`beginPanelOverride`, `src/d3d11/vscreen.cpp`). Curvature is the same
  interception with a bigger substitution: not one float in the
  transform, but the geometry itself — a flat quad cannot curve, whatever
  its transform says, so the draw must be re-issued over a finer mesh
  that is bent in the panel's own local space.
- **The cheap path needs no reverse-engineering of the transform.** Keep
  the game's own vertex shader, pixel shader and constant buffer, and
  substitute only the vertex/index buffers: a grid with the bend baked
  into its local-space positions. The game's shader then transforms bent
  geometry exactly as it transforms the flat quad — per eye, with
  whatever projection it uses — and EDVR never needs to know what the
  512 bytes of transform mean beyond the one float it already scales.
  Whether that path is open turns on one measurable fact: does the
  composite draw read a real vertex buffer in a local space, or generate
  its corners in the shader?
- **Two prerequisites are already visible in the code.** In HMD Cinema
  Mode only one of the two composite draws samples a panel-sized texture
  (`vscreen.cpp`, the `panelMiss` note: "one eye is corrected while the
  other is not") — a curvature that inherits today's recognition curves
  one eye, which is worse than not shipping. And a substituted draw
  touches far more pipeline state than a substituted constant buffer, so
  the save/restore discipline grows accordingly.

---

## What "curved" means, precisely

Virtual Desktop, Desktop+, OVR Toolkit and SteamVR's own dashboard all
mean the same thing by it: the flat image is mapped onto a section of a
vertical cylinder, concave toward the viewer, with the image's width
preserved as arc length. OpenVR exposes it for overlays as a single
number with a convention worth adopting outright
([`IVROverlay::SetOverlayCurvature`](https://github.com/ValveSoftware/openvr/blob/master/headers/openvr.h)):
curvature is the fraction of a full circle the bent screen occupies, in
(0..1], so for a screen of width *w* the cylinder radius is
*r = w / (2π · c)*. At c = 0.1 a 16:9 screen bends 36 degrees — a gentle
theatre curve; 1.0 wraps it into a closed cylinder. Vertical curvature is
not part of the feature anywhere, and not here either: the bend keeps
vertical lines vertical.

The geometry is a strip of quads: N columns across the width (N ≈ 64 is
far below visibility), each column's vertices displaced in the panel's
local space —

    angle(u)  = (u - 0.5) · 2π · c            u in 0..1 across the width
    x(u)      = r · sin(angle)                arc length preserved
    z(u)      = r · (1 - cos(angle))          toward the viewer
    y, UV     unchanged

— with c live-reloadable the way `panel_distance` already is, and c = 0
meaning the feature is off entirely, no substitution, no new code on the
hot path.

That is the whole of the maths. Everything below is about where those
vertices can be injected.

## Where the panel actually is in the pipeline

The facts EDVR has already established, all in `src/d3d11/vscreen.cpp`:

- On foot and in HMD Cinema Mode the world is rendered once, flat, into a
  16:9 panel texture (1920x1080 stock; `vscreen_res` can raise it). The
  panel then reaches the headset through the **panel composite**: for
  each eye, one draw into that eye's texture whose pixel-shader resource
  slot 0 samples the panel (`srv0IsPanelSized`).
- The composite's vertex shader reads a constant buffer of at most 512
  bytes in VS slot 0. EDVR shadows every write to it (the `Map`/`Unmap`
  hooks) and, for the distance fix, binds a copy with float index 47
  scaled, for exactly one draw, then restores the original
  (`beginPanelOverride` / `endPanelOverride`). The buffer is rewritten
  continuously while the panel is up — that is why capturing its writes
  works, and why a buffer that stops being written is treated as
  abandoned after 600 frames. Whether the two eyes' draws share one
  write or get one each is not established, and an existing census line
  already carries the answer (its `c=` token names the bound constant
  buffer per draw).
- What the 512 bytes mean is otherwise unknown. Index 47 is "the
  distance" because scaling it moves the panel and nothing else visibly
  changes (`advanced.panel_distance_index`); no matrix layout has ever
  been established, and nothing else in the tree needs one.
- The composite is recognised by what it reads, not by its shape or
  count: eye-sized render target (as published by the openvr half over
  the shared channel) plus panel-sized SRV0. On foot that recognition
  catches both eyes' draws. In HMD Cinema Mode it measurably does not —
  see prerequisites below.

Nothing anywhere in the tree records what geometry the composite draw
uses: no vertex-buffer hook exists, the census
(`draw_census.h`) records kind and index count only when armed, and no
census of a composite frame is checked in. That gap is the load-bearing
unknown of this whole feature.

## Path A: substitute the mesh, keep everything else

The premise: the composite draws a quad whose vertices live in a real
vertex buffer, in a panel-local space (corners plus UVs), and the vertex
shader multiplies them through the constant-buffer transform. Every
placed-surface draw in a conventional renderer looks like this, and the
distance fix's own success is weak evidence for it (a single scaled float
repositioning the panel smoothly is what a transform acting on
local-space geometry looks like) — but it is believed, not measured, and
Path A stands or falls with it.

If it holds, the substitution at the recognised draw is:

1. Save the IA state actually touched: vertex buffer slot 0 (pointer,
   stride, offset), index buffer, topology. `IAGet*` on the context
   answers all of it; none of those slots goes through an EDVR hook, so
   there is no shadow to confuse.
2. Bind EDVR's grid VB/IB — built lazily on the game's own device the way
   `ourCb` already is, in the same vertex format the game's quad uses,
   with the bend baked in — and issue the equivalent indexed draw with
   the grid's index count.
3. Restore the saved state, and do not forward the game's original draw.
   The existing `DrawVerdict::kSkip` plumbing (the census probe) already
   proves a recognised draw can be swallowed without the counting
   upstream noticing; this is a swallow-and-replace rather than a
   swallow.

The game's own vertex shader, pixel shader, input layout, samplers,
blend, depth and rasterizer state stay bound and untouched — that is the
entire attraction. The distance fix composes unchanged: the substituted
constant buffer (shadow copy, index 47 scaled) serves the substituted
mesh exactly as it serves the game's quad.

What Path A needs that does not exist yet:

- **The vertex format.** Learned once per session by capturing the
  game's quad: at a recognised composite draw, `CopyResource` the bound
  VB to a staging buffer, map it a few frames later, and read the
  handful of vertices. Stride comes from `IAGetVertexBuffers`. Four
  vertices of position-plus-UV are recognisable by eye in a log line and
  by code with a plausibility check (UVs in 0..1 at the corners,
  positions forming a rectangle). A layout that does not parse that way
  is a refusal: log what was seen, feature stands down for the session.
  This is the same posture `vscreen_res` takes about code it does not
  recognise.
- **A grid in that format.** Trivial once the format is known — the bend
  above, UVs interpolated linearly in arc length.
- **The staged proof.** Before any bend ships, a c = 0 substitution mode
  (flat grid replacing flat quad) must render pixel-identically. Any
  visible difference at zero curvature means the premise is wrong
  somewhere — format, winding, half-texel, topology — and it fails in
  the way that names itself, instead of shipping a subtle skew.

And one honest wrinkle even when everything holds: with the arc-length
convention, bent edges come nearer the viewer, so the panel's silhouette
on each eye changes shape. Anything the game draws after the composite
that depends on the panel's depth or footprint — the helmet furniture on
foot is drawn into the eyes afterwards, about 60 draws — could interact
(the census records whether a depth-stencil view is even bound at the
composite, so this is answerable from the instrument already shipped).
The distance fix moving the whole panel has never produced a report of
helmet-HUD misbehaviour, which is evidence the composite's depth is not
load-bearing, but a per-vertex displacement is a bigger perturbation
than a uniform one.

## Path B: own the draw outright

If the premise fails — the composite has no vertex buffer, and the
shader synthesises its corners from `SV_VertexID` plus the constant
buffer, the standard full-screen-pass trick — then the game's vertex
shader cannot be fed more geometry, and EDVR would have to draw the
curved screen itself: its own VS/PS (precompiled DXBC baked into the
DLL; the build has no shader step today and would gain an `fxc` pass),
its own input layout, and — the expensive part — its own understanding
of the transform, because its VS must place the mesh where the game
would have placed the quad.

That means finally reverse-engineering the 512-byte buffer: a diagnostic
that dumps the shadow as 128 floats on a hotkey, sessions correlating
which entries move with head yaw, pitch, position and `panel_distance`,
and a written-down layout the way `advanced.camera_buffer_offset` has
one — plus the acceptance that a game update can move it, detected the
way the distance fix detects it (the fix stops matching and says so).
The state save/restore also grows: shaders and input layout join the
list, though `VSGetShader`/`IAGetInputLayout` make it mechanical.

Path B is strictly more code, strictly more session time, and adds a
per-build fragility Path A does not have. It is the fallback, not the
plan. The one thing it buys that Path A cannot: EDVR-authored placement,
which would also unlock screen *size* and *height* settings. Not asked
for; noted and left.

## The alternative considered and rejected: an OpenVR overlay

SteamVR itself curves surfaces every day — dashboard, Virtual Desktop's
own screen — through
[`IVROverlay::SetOverlayCurvature`](https://github.com/ValveSoftware/openvr/blob/master/headers/openvr.h).
The tempting design: capture the panel texture (recognised already),
hand it to an overlay with curvature set, suppress the composite draws
(the census probe proves that works), and let the compositor do the
bending. It fails this project's constraints three ways:

1. **The user base it would serve least is the one EDVR has.** Overlays
   require a runtime that implements IVROverlay. The ini's own comment
   on `advanced.suppress_interfaces` records that OpenComposite raises a
   FATAL dialog for interfaces it does not implement, with IVROverlay as
   the worked example — and both rigs this project's fixes were field
   verified on (docs/dxvk-windows.md, unknown 5) are OpenComposite-family
   stacks. A curvature that works only on real SteamVR inverts the
   project's actual audience.
2. **Composition order is wrong on foot.** Overlays composite over the
   submitted eyes. The helmet furniture is drawn into the eyes after the
   panel, so a curved overlay would sit in front of it. Clean in HMD
   Cinema Mode (the eyes hold panel plus void and nothing else), wrong
   exactly where users spend their on-foot time.
3. **It calls into the runtime, which this project deliberately never
   does.** The IVRSystem_012 stack-cookie crash (`compositor_hook.cpp`,
   top) is why every EDVR interface interaction is hook-and-observe,
   never call. The game links IVROverlay_011, which predates the
   curvature call by years; EDVR would have to request a modern
   IVROverlay from the runtime and call a dozen methods on it by
   declaration-order index — the exact shape of risk the codebase's one
   hard rule exists to refuse.

A screen-space warp at Submit was discarded faster: bending is a
3D repositioning, not an image distortion — warping the flat image in
place gets the stereo wrong per eye and fights the runtime's
reprojection, and the panel's footprint within the eye texture is not
knowable at Submit anyway.

## Prerequisites, named as work

1. **The Cinema Mode half-recognition.** Today's recogniser catches one
   of Cinema Mode's two composite draws; the other samples something
   that is not panel-sized, sizes already logged by the `panelMiss`
   note (`vscreen.cpp`). For the distance fix this is a wrong-distance
   eye; for curvature it is a curved eye and a flat one, which is not
   shippable. The fix worth building is the recogniser the curvature
   work makes possible anyway: once the composite's shaders are
   identified (the device hook already fingerprints shaders at creation
   — `registerShaderHash` in `device_hook.cpp`, today only for compute;
   `CreateVertexShader` is the same pattern at device slot 12), "draw
   into an eye target using the composite shader" recognises both eyes
   in both modes, and stops depending on SRV size — which also retires
   the panel-equals-eye-size collision the log currently has to warn
   about.
2. **The composite census.** One armed census of a Cinema Mode frame and
   an on-foot frame, plus two added fields worth recording for eye draws
   while armed: vertex buffer (bound or not, and its stride) and
   topology. The DSV and the constant-buffer identity are already in
   every census line, so one session settles Path A vs Path B, the depth
   question, the one-buffer-or-two question, and the second eye's
   source. The instrument is already shipped; this is extending its line
   by two tokens and reading it.
3. **State save/restore discipline.** The substitution touches IA state
   the current fix never has. The restore must be exact under the same
   adversaries the file already documents: `ClearState` mid-frame,
   deferred-context replays, foreign contexts (all already handled at
   the recognition layer — the substitution inherits that for free, but
   the restore path is new code on the same hot path).

## Config surface, in this file's own idiom

    [fix]
    # Bend the on-foot screen (and HMD Cinema Mode) toward you, like
    # Virtual Desktop's curved screen. The number is the fraction of a
    # full circle the screen occupies: 0 is flat and off, 0.1 is a
    # gentle theatre curve, 0.25 wraps a quarter circle around you.
    # Takes effect within a second, no restart. Composes with
    # panel_distance.
    panel_curvature = 0.0

    [advanced]
    # Columns in the substituted mesh. Only worth touching if a log line
    # asks.
    #panel_curvature_segments = 64

Live reload rides `vScreenRefreshConfig` exactly as `panel_distance`
does, and `tools/check_config_contract.py` holds the ini, the README and
the reader together as it does for every other key.

## What only running it can settle

In this codebase's idiom: things this design *believes* and has not
*measured*, each with the failure it would produce.

1. **The composite reads a real vertex buffer in local space.** The
   premise of Path A. *Failure mode: the capture finds no VB or an
   unparseable layout; the feature refuses for the session with a log
   line, and Path B becomes the plan.*
2. **The transform is projective per eye, so a local-space bend reads as
   depth.** If the shader's transform turned out to be a flat 2D
   mapping with per-eye offsets, the bend would show as a 2D distortion
   with no stereo depth — visible in one headset session at c = 0.2.
   The panel's parallax behaviour makes this unlikely; unlikely is not
   measured. *Failure mode: looks wrong immediately, feature does not
   ship.*
3. **Zero-curvature substitution is pixel-identical.** The staged proof
   that format, winding, topology and half-texel details are all
   understood. *Failure mode: any visible difference at c = 0; named
   check fails before a bend is ever attempted.*
4. **The helmet furniture tolerates the bend.** Whether anything drawn
   after the composite depends on its depth or silhouette. *Failure
   mode: HUD elements clip or float; measurable from the census's DSV
   field before writing any code.*
5. **The second Cinema Mode draw is what prerequisite 1 assumes** — a
   composite by another source, catchable by shader identity. The
   `panelMiss` sizes in existing field logs are the first thing to
   read. *Failure mode: Cinema Mode stays half-recognised; curvature
   ships on-foot-only with the Cinema limitation stated, or waits.*
6. **Perf is a non-event.** ~130 vertices against draws the game already
   makes, twice a frame; the only new steady-state cost is the ordinary
   recognition already paid for the distance fix. Stated to be measured,
   not because doubt is real.

## Recommendation

Phased, each phase shippable alone, in this project's pattern:

- **Phase 0 — read what is already written.** The `panelMiss` lines in
  existing field logs (what the second Cinema eye samples), one census
  of each mode as shipped (the DSV, constant-buffer and SRV tokens are
  already in every line), and the census extended by the two IA tokens
  for one more session. Nothing user-visible changes; settles unknowns
  1, 4 and 5 and chooses the path.
- **Phase 1 — recognise by shader.** The `CreateVertexShader` hash hook
  and the composite-shader recogniser, shipped alone: it fixes the
  Cinema Mode one-eye distance bug and retires the size-collision
  warnings, which is worth releasing with no curvature attached — and
  it is the recognition curvature requires.
- **Phase 2 — substitution at zero.** The capture, the format check,
  the grid, the save/restore, behind `panel_curvature` — but validated
  entirely at c = 0 against the pixel-identity bar, plus the existing
  smoke coverage for the new state handling.
- **Phase 3 — the bend.** The two-line displacement, a headset, and the
  comfort tuning pass; README section and release the way `panel_distance`
  earned its own.

The case for doing it: it is the one comfort feature users name that the
project's existing mechanism — recognise the composite, substitute for
one draw, restore — was effectively built for, and Phase 1 pays for
itself even if the bend never ships. The case against is that the
decisive facts (the vertex buffer, the second Cinema draw) are
unmeasured, which is exactly why the phases put the measuring first and
the geometry last.

---

## Phase 0, in progress

*Everything above this line is the design as first written. Everything
below is what happened when it met the logs.*

### The `panelMiss` lines cannot answer unknown 5

Phase 0's first step was to read what the shipped instrument had already
written. 339 field logs hold the note, and it does not say what the
design assumed it says.

The note fires inside `srv0IsPanelSized`, which runs for **every**
eye-sized draw whose slot 0 is not the panel — the helmet HUD, glyph
sheets, post-process scratch. It records the first eight distinct
sampled sizes of a session and then stops. So a session fills its eight
slots long before a composite is reached, and what the logs actually
hold is the shape of the cockpit:

    512x512   1920x1080   16x16   0x0   1x1   128x128   4184x4132   3285x1847

Those are not candidates for what the second Cinema eye reads. They are
whatever the frame happened to sample first. The line was written to
answer this question and never could: it has no way to tell a composite
from a HUD quad, because it never saw the draw's shape.

It does now — the note names the draw's kind and vertex count, so a
composite (a handful of vertices) is separable by reading from the
helmet (thousands). That narrows unknown 5. What settles it is the
census.

### What the census now records

The design asked for the census line to gain two IA tokens. It has
gained four, read straight off the context with `IAGet*`/`VSGetShader`
while a census is armed — no new hooks, nothing on the hot path,
nothing further for the D3D runtime to re-point:

    ... s=@4,-,-,-  vs=@7 vb=@8 sd=32 of=0 tp=4
                    |     |     |     |    `- D3D11_PRIMITIVE_TOPOLOGY
                    |     |     |     `- byte offset into the buffer
                    |     |     `- vertex stride; 0 means no vertex buffer
                    |     `- the vertex buffer's identity, with its byte
                    |        width in the id table at the end
                    `- the vertex shader's identity

`c=`, the transform buffer, now resolves to its byte width in that id
table too, rather than staying an opaque ordinal — so a census states
the composite constant buffer's real size instead of leaving "at most
512 bytes" an inference.

Absent tokens mean *not measured* (the probe faulted, or its budget is
spent), never *nothing was bound*, which has its own spelling, `-`.

### How to capture it

The instrument is in the build; it needs one session. Two censuses, read
directly rather than diffed — `diff_draw_census.py` isolates an effect
that toggles, and this is not one.

1. Bind the census key, in the **game directory's** `edvr.ini` (not the
   repo's; that is not the copy the game reads), under `[hotkey]`:

       dump_draws = NUMLOCK

   A bare key: chords on the Pause/ScrollLock cluster reach Windows as
   Break and never fire. The log says what the key bound, so check there.

2. **On foot**, standing still, panel up: press it once. Three whole
   frames record.

3. **HMD Cinema Mode**, the same, once more.

4. Send the log.

### What the two censuses settle

Read the lines whose `s=` slot 0 is the panel's size — those are the
composite draws the distance fix already recognises — and then the lines
around them.

| Question | Where the answer is | What it decides |
|---|---|---|
| Unknown 1: does the composite read a real vertex buffer? | `sd=` on a composite line | Non-zero, with a small `vb` byte width, and **Path A** is open. `sd=0` with `vb=-` and the shader synthesises its corners, which is **Path B** |
| Which vertex format | `sd=` with `tp=` | Sizes the capture Phase 2 needs, and says whether the quad is a list (`tp=4`) or a strip (`tp=5`) |
| Unknown 4: is the composite's depth load-bearing? | `d=` on a composite line | `-` means nothing drawn afterwards can depend on the panel's depth, and the bend cannot disturb what is not there |
| Unknown 5: do Cinema Mode's two composite draws share a shader? | `vs=` on both eyes' lines in the Cinema census | The same ordinal, and recognising by shader catches the eye the SRV test misses — **Phase 1 is the fix**. Different, and the second draw is something else, to be identified before curvature can be whole |
| One transform buffer or two | `c=` on both eyes' lines | Whether the shadow the distance fix keeps belongs to one buffer or two |

Two of those choose between Path A and Path B; one decides whether Phase
1 is worth shipping on its own. None can be settled from a desk, which
is why nothing past this point is written yet.
