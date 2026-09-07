# Handoff: crisp UI under DLSS

*Status: DESIGN, nothing built. Written 2026-09-05 on branch
`claude/ui-text-aliasing-dlss-dd8735` off main `2ff1267` (v0.14.0 plus docs).
For the agent who builds it. Everything marked MEASURED comes from a flight
log, a census or a disassembly kept in this repo; everything marked BELIEVED
is inference, and each belief has a gate below that turns it into a
measurement before code is allowed to depend on it.*

## The short version

Under `temporal_aa = dlss` (and, less visibly, `dlaa` and `on`) every piece of
Elite's UI flickers and swims: the cockpit's holo panels, the flight HUD, the
main menu's panel, the loading screen's text. Sean's depth view shows why:
none of it writes depth. The pass reprojects a depthless pixel at the far
plane, which is wrong for a panel a metre away under head translation, NVIDIA's
history rejects it there, and what shows through the rejection is the raw
jittered frame -- a sub-pixel shuttle at the jitter period, read as flicker.
On top of that, at DLSS Quality the UI is RASTERISED at two-thirds resolution,
because Elite's interface surfaces track the internal render size, so there
is less detail in the text than the headset can show before any of the above
happens.

Two designs are written up here. **Design A, the UI layer,** is the textbook
answer and the one to build first: recognise the UI draws, redirect them into
an EDVR-owned layer at the pass's OUTPUT size with the jitter cancelled, and
composite that layer over the finished frame as the last step at the door.
Text is then drawn once, at the headset's resolution, unjittered, and never
passes through the upscaler at all, so it cannot swim or ghost by
construction. **Design B, the depth layer,** is the fallback: re-issue the
same draws into an EDVR-owned depth target so the pass and NVIDIA reproject
the UI at its real distance, which makes NVIDIA accumulate the text the way it
already accumulates the cockpit. B is a third of A's code, keeps the game's
own look exactly, and leaves the text softer than A.

Both need the interface surfaces rasterised at the output size to give the
text real detail; that lever exists (`advanced.surface_inflate`, built in the
target-indicator arc and field-verified engaging) but needs a float factor
and one flight to prove it reaches the TEXT and not only the vector lines,
because no inflate flight has ever been judged on the letters (A5, gate G9).
Sean's question -- could the UI be CAS-upscaled
independently -- is answered in A5: yes, the machinery for it exists
(`panel_upscale`), but a spatial upscale of a two-thirds rasterisation adds
no detail, only cleaner edges, and it does nothing about the swim. Inflation
does the resolution half properly and A or B does the temporal half; CAS is
the weaker substitute for the first and no answer to the second.

Nothing here is built. The design rests on eight BELIEVED facts that one
census session settles (the gates section), and the order of work is: run the
gates, build A behind a default-off key, fly it with the layer-only debug
view, then decide whether B is needed at all.

## The ask, and its two halves

Sean, 2026-09-05: under DLSS "the text aliasing suffers, often introducing
unwanted flickering/swimming on UI surfaces. These surfaces do not appear to
have depth to them according to the depth debug mode." Then: "this is not
just the hud/cockpit ui, but all ui surfaces (menus, loading screen text)."
And: "if we're using DLSS, likely we're rendering the UI text at a lower
resolution, would it be possible to independently scale them using CAS or
similar to make them appear crisp for the target resolution?"

The symptom has two independent causes, and each needs its own fix.

**Half one, resolution.** MEASURED (`src/d3d11/fss_res.h`, three render
sizes, ratios stable to four figures): Elite builds its 2D UI in offscreen
INTERFACE SURFACES whose size is a fixed fraction of the INTERNAL render
resolution. The game's own vector shader (vs `666EF0C4C616F67E`, whose pixel
shader samples nothing), its text shader (`1012E00B3CB44469`, a 2048x2048 A8
glyph atlas) and its icon shader (`A3E5D3FCBC1165F8`, small BC7 pages)
rasterise into those surfaces, and a mesh in the eye then samples the surface
onto a panel. Under DLSS Quality the game renders at about 0.67 of the
unit-quality size, so every surface is two-thirds the size it was at HMD
Quality 1.0, and every glyph is rasterised with two-thirds the pixels. The
review of 2026-09-04 recorded this as its F3: "the interface surfaces scale
with INTERNAL res, so DLSS buys frame time, not text."

**Half two, the temporal treatment.** MEASURED (flight 29, 2026-09-04, the
depth debug view): "NONE OF THE COCKPIT UI PANELS OR HUD TEXT REGISTER WITH
DEPTH ANYWHERE. Elite draws the HUD on top, depthless." The main menu's panel
and the loading screen's text read the same way. The pass's motion vectors are
depth-reprojected head motion (`docs/anti-aliasing.md`, Feature B, v2); a
pixel with no depth falls on the far-plane path, which follows head ROTATION
exactly and head TRANSLATION not at all. A panel a metre away moves about two
pixels a frame at a 30 deg/s head sweep on a 3096-wide eye (the flight 2
arithmetic), plus a fraction of a pixel from tracking noise while "still".
The history is therefore misregistered exactly on the UI, NVIDIA's history
rejects it there, and the current frame shows through -- jittered, so it
shuttles by up to half a pixel at the Halton period. That is the flicker.
Where the rejection is partial the result is the smear Sean calls swimming.

Sean's CAS idea addresses half one only, and weakly (A5). A UI element that
never touches the upscaler needs neither half fixed by the upscaler, which is
what design A does. Design B fixes half two only, and needs A5 for half one.

## What is measured, and what is believed

| Claim | Status | Where |
|---|---|---|
| The UI writes no depth; the depth view paints it magenta | MEASURED | flight 29, `edvr.ini` comment on `temporal_aa_debug` |
| Interface surfaces track the internal render size | MEASURED | `fss_res.h`, three sizes, three surfaces |
| Inflating a surface at creation makes the game rasterise into it larger, viewports scaled to match | MEASURED | the target-indicator arc, 2026-09-02: six textures created, six viewports scaled, frame rate unchanged |
| Inflating a surface behind the game's back sharpens its TEXT, not only its vector lines | BELIEVED | both inflate flights were judged on the fixed-size target indicator alone; the glyph atlas's lifetime is unknown -- G9 |
| Cockpit holo panels: one family, vs `81216C77F90DEDD6` / ps `A2965EC2931A39C8`, 24 draws a frame, each sampling a different surface at PS slot 2 | MEASURED | `panel_upscale.h`, the cockpit-HUD map of 2026-09-02 |
| A second composite family exists, vs `E508648660A352B2`, sampling one surface (magnified: `uv = snorm16 * cb2[1].xy * 16 + cb2[1].zw`) | MEASURED | `hud_sprite.h`, the target-indicator hunt |
| The flight HUD (altimeter ladder, speed, coords) is vs `B7790CBFC6554097` / ps `8DEF46452FA459F5`, vector geometry straight into the eye, sampling the eye-sized depth at PS slot 0 and a 256x256 noise table at slot 1 | MEASURED | `hud_grain.cpp:137-161` |
| The target direction indicator is one 6-vertex quad per eye, vs `5DA53D8B0133341E`, sampling a 1024x256 BC7 atlas | MEASURED | `hud_sprite.h` |
| The loading dialog goes through an interface surface and "the 5760-index composite that lifts the interface in"; its dark backing is a separate eye-level quad with depth | MEASURED | `loader_panel.h` |
| The on-foot screen composite projects a 3D-placed quad through cb1[270..273], the eye's projection rows | MEASURED | `docs/shaders/composite-vs.asm` |
| The pass's jitter is a shift of all four frustum tangents, told through the raw thunk and the matrix receiver alike | MEASURED | `system_hook.cpp:465-474`, `:597-619` |
| Elite submits ONE double-wide texture with per-eye bounds (2912x1560, each eye u 0..0.5 or 0.5..1, on 2026-08-17) | MEASURED then | `supersample_math.h`; the 2026-09 logs quote per-eye REGION sizes, which the region maths produces from either shape, so which shape the UI draws' render targets take is still open -- G5 |
| The UI composites are drawn into the tonemapped 8-bit target, AFTER the HDR chain | BELIEVED | the RemLok overlay is (`docs/remlok-lines.md`); the holo panels are not yet censused for it -- G1 |
| The UI composites carry the eye's (jittered) projection | BELIEVED | the on-foot composite does; the holo panel family is a 470-instruction mesh shader nobody has read -- G4 |
| The UI draws have no depth view bound, or depth testing off | BELIEVED | consistent with flight 29; the census's `ds=` field settles it -- G2 |
| The GUI families never draw directly into an eye target | BELIEVED | every direct case found so far went through a surface -- G6 |

## Why the earlier decline no longer applies

`docs/anti-aliasing.md`, "MSAA from outside, and why the answer is no",
considered exactly this shape -- "redirect those draws (recognised by shader
hash, the census's currency) into an EDVR-owned target ... resolve, and
composite back with the game's own blend" -- and declined it because
"features A and B below anti-alias the HUD *and* everything behind it with
none of that." That was true of the box-filtered resolve and of the pass's
own history. It is not true of the trained modes: DLAA and DLSS cannot
register a depthless panel, and their answer to misregistration is rejection,
which is where the flicker comes from. The premise changed, so the decline
should be revisited, and this document is the revisit. The section "The
order at the door" in that doc gets one new last step when A ships.

The programming guide's own position, quoted in the same doc: DLSS wants to
run before UI compositing. Every injected DLSS runs post-HUD and lives with
it. This is the one place EDVR can do better than an injector, because it
already sits on the draw path.

# What the first censuses said (2026-09-06)

Before any new flight, four cockpit censuses from 2026-09-03 (the
cockpit-HUD arc's, Quest 3 at 2064x2208, offscreen draws recorded) were
read with `tools/crisp_ui_gates.py`. They answer G1, G2, G3, G6 and G8, and
a gate this document had not asked, G10 -- and they change the order of
work.

| Gate | Answer | Evidence |
|---|---|---|
| G1 | The holo panels and the flight HUD are drawn into the lit HDR buffer, `R11G11B10_FLOAT`, BEFORE exposure, tonemap and SMAA. The target indicator quad goes into an `RGB10A2_TYPELESS` target. | the family lines' `r=`; the tonemap `2D78DC3FD2C0C543` is the only reader of the R11G11B10F target after the last UI draw, and SMAA (`68842760565CC3BA` edges, `03D186CE0EC031E3` weights with the 160x560 area and 64x16 search textures, `98E6F9986FDC9A53` blend) runs after it on the RGBA8 target |
| G2 | The UI draws bind the scene depth pair and TEST against it (`GEQUAL`, reversed-Z), write off. Not depthless: depth-tested and non-writing. | `ds=17wZ` on every holo-panel, flight-HUD and target-indicator line |
| G3 | ONE/INV_SRC_ALPHA (premultiplied over) for the panels and the flight HUD; SRC_ALPHA/INV_SRC_ALPHA for the indicator. All within the layer's table. | `bl=` |
| G6 | No GUI family ever drew into an eye target. | none in four censuses |
| G8 | After the UI: the exposure compute shader, the tonemap into `RGBA8_TYPELESS`, SMAA, a copy into an `RGBA8_UNORM_SRGB` texture (the submit), the monitor mirror. The stock HUD is SMAA'd. | the frame tails |
| G10 (new) | After the last UI draw nothing tests against or reads the scene depth, except two overlay draws (`A888D51024D9798E`) whose depth test is OFF. | per complete frame, censuses with identity tokens |
| G5 (partial) | Per-eye targets: one R11G11B10F and one RGB10A2 per eye, one depth pair per eye. | the tokens |

**What follows.**

1. **Design A as written is out for the cockpit.** A layer composited at
   the door takes the panels out of the HDR chain: no bloom, no tonemap,
   no SMAA, and the look changes. A's viable form transcribes the tonemap
   (`2D78DC3FD2C0C543`, fed by the exposure shader the exposure fix already
   reads) and applies it to the layer, accepts the missing bloom, and adds
   the layer's own edge anti-aliasing. That is a larger arc and is PARKED,
   not declined: the classifier, the viewport map and the composite are
   still the right pieces if it is ever built.
2. **Design B has a smaller form than the one written below, B0.** The UI
   draws already bind the scene depth and test against it. Swapping their
   depth-stencil state to WRITE, with the game's own GEQUAL test kept, puts
   the HUD's depth into the scene pair -- where the pass, the depth probe
   and NVIDIA's depth copy already read it, with no pairing, no copy and no
   pass change. G10 says nothing downstream reads that buffer after the UI.
   About forty lines: the classifier plus one state swap in the wrap.
   Two hazards decide between B0 and the copy form: (a) with the test kept,
   a UI element drawn AFTER another at a greater distance loses its overlap
   where the earlier one wrote depth, and whether a panel's transparent
   area stamps at all turns on its pixel shader discarding on alpha; (b)
   see-through pixels take the panel's motion, as B4 says. B0 flies first
   because it is cheap; B with the copy (a per-eye copy of the scene depth
   at the eye's first UI draw, the re-issue into it with the test kept, the
   pass reading the copy) is exact and leaves the game's buffer alone, and
   is the fallback if either hazard shows.
3. **The classifier stands.** Five surfaces learned per session, every
   holo-panel draw found through slot 2, the direct list confirmed, G6
   none. It is the same code for A, B and B0.

**Plan.** Flight 1: the three censuses (main menu, loading screen, cockpit)
on the Frontier install with the `vf=` build and the shader dump, to settle
G1's view format, G5 and G6 outside the cockpit, and G10 in the menus. Build
B0 while it flies. Flight 2: B0 on against off, docked and in space. G9 (the
surfaces' resolution) after that, since B0 changes nothing about detail.

## Flight 1 (2026-09-06, Frontier install, Pimax at 2818x2784 under DLSS)

Five censuses landed before the graphics log hit its 4 MB cap (raise
`log.max_mb` before the next census session): the main menu, the loading
screen, and three cockpit views. Read with the reader, with the 2048-entry
table so every token kept its identity:

- **Main menu:** the panel is one GUI surface (2212x1244), composited by
  `A888D51024D9798E` / `9107E72CB016CC02` into the tonemapped `RGBA8_TYPELESS`
  target through a UNORM view, depth test OFF, premultiplied blend. After
  it: only the copies to the submit texture and EDVR's own pass.
- **Loading screen:** the same composite family plus the loader's
  5760-index `4EF6DDB075A927FA` and a `B018D143700AB803` quad, sampling two
  2212x1244 surfaces, into the 8-bit target, test off. The reader saw no GUI
  draws in its three frames because the dialog's surfaces are rebuilt only
  when they change: the DLL's session-long memory is what classifies them.
- **Cockpit (three views):** exactly the 2026-09-03 picture at the new
  size. Holo panels 22-24 a frame and the flight HUD into `R11G11B10F`
  (`view=same`), test GEQUAL write off, premultiplied; the target indicator
  quad into `RGB10A2_TYPELESS` (view `RGB10A2_UNORM`), which the loading
  census shows to be the G-BUFFER colour target (the ship model draws into
  it with depth write on, before the lighting resolve) -- so that family
  is excluded from B0. After the last UI draw: the tonemap reads the HDR
  target and nothing else; the only touch of the depth is EDVR's own mv
  dispatch (`6D94E9C00DCE909F`, reading it as `R32F_X8X24_TYPELESS`).
- **G1's view format:** the HDR target is `R11G11B10F` viewed as itself;
  the 8-bit target is `RGBA8_TYPELESS` viewed as `RGBA8_UNORM` (encoded-space
  blending) at every UI composite in the menus.
- **The shader dump answered the discard question.** `ps A2965EC2931A39C8`
  (holo panels) discards at two sites, `ps 8DEF46452FA459F5` (flight HUD)
  at five, the menu/loader composites at one each; the target indicator's
  `ps E23C45251B7ECDFE` at none. Every composite vertex shader projects
  through `cb1[276]`, the scene block's projection rows, so they carry the
  jitter (G4, for A).

**Built from this: `fix.ui_depth` (src/d3d11/ui_depth.h), the B0 form.**
The classifier as designed (GUI-family surfaces learned at offscreen draws,
composites by what they sample, the flight HUD by hash, an exclude list),
and one state swap per UI draw: the game's depth-stencil state's writing
twin, its test kept, ALWAYS where it had none. Default off; ini only until
flown.

## Flight 2 (2026-09-06 11:21, v0.14.0-15-gaa353b5, the on/off A/B)

**Sean: "That looks much better!"** -- the text stopped shuttling and
swimming under DLSS with `ui_depth = on`, toggled live against off, docked
and after a jump. The log:

- engaged on the first frame after the switch; then 63 to 82 interface
  draws a frame writing depth (24 to 28 composites, the rest the flight
  HUD), 9 surfaces known within a minute and 13 by the end as panels came
  into view; 5 depth states derived; nothing left alone for want of a
  writable state; no fault, no stand-down;
- the pass unchanged in price: NVIDIA's 1.85 ms an eye, EDVR's 2.34, 88-90
  fps, and NVIDIA's history restarted 4 times in 40,000 eye-frames -- the
  interface's new depth did not disturb it;
- the depth probe's cockpit-layer targets read 238-243 of 256 samples
  within 2 m, as before.

Two counter fixes followed the read (the "left alone: no depth target"
figure had counted every depthless eye draw, 10 a frame of post chain; the
window counters did not reset on a live toggle). Neither changes behaviour.

**What this settles.** The temporal half of the defect was the depth, and
B0 is enough for it. Design A stays parked. What B0 does not touch is
detail: the letters are still rasterised at the render size, and G9 is the
next flight -- no build, an ini line:

    surface_inflate = 1267x1036:2, 589x883:2, 1327x760:2, 1769x380:2

which names the four text-carrying cockpit surfaces measured at 2818x2784
(HMD Quality 0.65 on the Pimax; the sizes follow the internal render size,
so the same HMD Quality must be used), inflated 2x from the next launch.
Judge the letters against the same panels with the line commented out.
The 2654x2322 surface carries icons and vectors only and is left out; the
menu and loader share 2212x1244.

**Open, small:** the target indicator quad still has no depth (it draws
into the G-buffer before lighting, so writing there would feed the
lighting resolve); the loading screen's UI has depth only when the depth
probe finds a scene pair there, which its draw-count rule refuses on the
loader's few draws.

## The adversarial review (2026-09-06) and what changed

An Opus 5 agent reviewed the built fix against the code and the censuses
(`docs/review-ui-depth-2026-09-06.md`). Verdict: no crash or hang, the
mechanism sound where measured, the cockpit evidence good; eighteen
findings, five of them blocking a default-on merge. Every one was taken:

- **The restore is never skipped.** `uiDepthEnd` restored inside
  `guardedBudget`, which skips its body once the budget is spent, so a
  budget spent between Begin and End would have left the writing twin bound
  for every later draw. Now `guarded` (SEH, no budget), the way
  `resolve_probe.cpp` restores.
- **Gated on the pass.** `fix.temporal_aa = off` now makes the module do
  nothing, so the default costs a stock install nothing (the depth probe's
  own rule). A key set on without the pass says once that it waits.
- **Writes only where the pass reads.** The menu's composite binds a depth
  of its own that nothing reads (measured: no event in the menu census
  touches it, while the pass's dispatch reads the hangar's pair). Such
  draws are now left alone and counted ("not into the scene's depth" on
  the totals line), through `depthProbeIsSceneDepth`. The prose in the ini
  and the README claims the cockpit and the flight HUD, which is what was
  flown.
- **The memos fit and cost nothing.** A 256-entry FIFO memo against a
  measured 257-275 sampled views a frame thrashed, pushing thousands of
  pointers a frame through `bindingResolve`, whose five-fault budget is
  shared with `targetIsEyeSized`. Now a 1024-entry hashed memo with a
  four-slot probe, and a second one for vertex-shader hashes so the
  registry's lock is not taken per draw. Cheapest test first again: no
  depth target, then the surfaces, then the hash.
- **Learned surfaces are validated and evicted.** Each surface is kept
  with the size and format it had when learned; a sampled resource at a
  learned address with a different shape is a recycled address and drops
  out, counted. The ring's overwrites are counted and said once.
- **The state cache checks its room before creating.** A full table used
  to create and release a D3D state per draw for the session.
- **The header says what is true.** The indicator is left alone because
  it samples an authored atlas and lookup tables, not a learned surface;
  there is no target check. The visibility the header promised now exists:
  every newly classified family logs one line with its target's size and
  format, so a field log from the galaxy map or on foot can name what was
  treated.
- **The per-draw flag is thread-local**, so a draw recorded on a deferred
  context by another thread can neither take the render thread's flag nor
  be treated by it.
- `advanced.ui_depth_test = always` is the A/B for the kept test's one
  cost (a later, farther piece of interface losing its overlap against an
  earlier, nearer one). Counters split "already writes" from "no twin"; a
  stand-down is for the session.

Not taken, recorded: the reviewer's ladder ([experimental] first, default
on after a menu, a loading screen and an un-censused context have been
flown). Sean chose default on after the cockpit flight, with the gate and
the scene-pair check making the default free and explicit elsewhere. The
"about forty lines" of the plan became about 450 with the classifier's
memos and the logging.

## The menus and the loading screen: the rebind (2026-09-07)

Sean, after the merged build: the cockpit UI "looks great", the main menu,
the settings menu and the loading screens are still aliased and swimming.
Expected from the review's finding 3: those composites bind a depth target
of their own that nothing reads, while the pass reads the busiest pair (the
hangar's in the menu; the ship model's on the loading screen), so a depth
written where the game put it registers nothing.

The fix is to write it where the pass reads. For a classified draw whose
bound depth is not the scene pair's, the module binds EDVR's own depth view
over the pair's texture for that eye in the game's place for the one draw
(`depthProbeSceneDepthFormat` hands out the probe's texture and the view
format the game binds it with; `vScreenSetRenderTargetsRaw` goes through
the original OM entry so the binding shadow keeps describing the game's
bindings), writes with the ALWAYS twin the test-off state derives to, and
puts the game's targets back before anything else looks. The eye is read
from the order the draw's colour target first appears among treated draws
in the frame -- first is the left, the depth probe's own rule for the pair
-- with `advanced.ui_depth_eyes = swapped` as the A/B if a menu steadies in
one eye only, and `advanced.ui_depth_menus = 0` as the kill switch. The
totals line counts the rebound draws and the draws with no pair to bind.

What it rests on that is not yet measured: that the LDR composites appear
in the same eye order as the G-buffer passes the probe orders the pair by
(a swap degrades to a half-fixed panel, since both eyes' panels sit at the
same depth and only their screen positions differ); and that a composite's
`SV_Position.z` is a real distance, which the shader dump supports (every
composite vertex shader projects through the scene block's rows) and a
2D quad placed in NDC would break -- none of the classified families is
one. Unflown at the time of writing.

# Design A: the UI layer

## A1. What counts as UI: the classifier

Sean's scope is every UI surface, so a list of composite hashes is the wrong
currency: the main menu, station services, the galaxy map, the loading
screen and the on-foot screen each have their own composite, and a list
grows one flight at a time. The stable fingerprint is the GUI renderer
itself. Three shader families draw every piece of 2D UI Elite has, wherever
it ends up:

| Family | Hash (build 332753) | What it draws |
|---|---|---|
| vector | vs `666EF0C4C616F67E` | panels, lines, the 30-index bordered quad set, the wake ring |
| text | `1012E00B3CB44469` | glyphs from the 2048x2048 A8 atlas |
| icons | `A3E5D3FCBC1165F8` | small BC7 icon pages |

(All three are pinnable the way `panel_upscale` pins its hash; a game update
can recompile a shader without changing what it does.)

The classifier has three rules, in this order:

1. **A UI surface is any render target one of the GUI families has drawn
   into.** Learned at the OFFSCREEN branch of vscreen's draw path (the branch
   that already calls `drawCensusOffDraw`, `vscreen.cpp:1553-1576`): when
   the bound vertex shader hashes to a GUI family and the target is not an
   eye texture, remember the target's RESOURCE identity (the binding
   shadow's `ResourceInfo::resource`, compared never dereferenced). The set
   persists for the session: some surfaces are rebuilt every frame (the
   cockpit panels, 83 draws each) and some only on an event (the target
   indicator's surface was rebuilt after 5091 quiet frames), so a surface
   stays a surface until its texture dies. Evict on a size change of the eye
   (the game recreates everything) and when the shadow's resolve of the
   pointer fails.

2. **A UI composite is any EYE draw whose pixel stage samples a UI surface
   in slots 0-3.** The binding shadow already tracks `PsSrv0..3`
   (`binding_shadow.h`) for the census, so this is a few pointer compares per
   eye draw, cached per binding generation the way `rtv0Eye` is. This one
   rule catches the holo-panel family (slot 2), the `E508648660A352B2`
   composite, the loader's 5760-index composite, the menu's panel, and the
   on-foot screen composite, without naming any of them.

3. **A direct UI draw is an eye draw by a family on the direct list.**
   Shipped list: the flight HUD `B7790CBFC6554097` and the target indicator
   `5DA53D8B0133341E`; `advanced.crisp_ui_families` adds hashes, and a GUI
   family drawing straight into an eye target counts automatically if G6
   ever finds one. `advanced.crisp_ui_exclude` removes hashes; ship it empty.

Two guards that the cockpit-HUD arc paid for:

- **Rule 2 fires only on eye targets that the game SUBMITS.** "Eye-sized" is
  not enough (`vscreen.cpp:2167` says why: hundreds of draws land in
  eye-sized textures). The eye target set is: the texture handed to Submit
  (the d3d11 half sees it as `srcTex` in every treat), plus any texture the
  game copies INTO it this frame (the copy hooks at `vscreen.cpp:2855` and
  `:2933` already tee every copy to the census; note source resources whose
  destination is a submitted texture). Learned from the previous frame; the
  set is stable. Without this guard an eye-SIZED interface surface (a
  full-screen station-services panel, say) would have its GUI draws
  classified as direct UI and redirected, leaving its composite sampling an
  empty surface.
- **Rule 2 must not fire on a draw whose verdict swallows it.** `kSkip`,
  `kQuadSkip` and the loader panel's swallow are withholds; a withheld draw
  has no pixels to redirect. The wrap composes with the verdicts that
  FORWARD (A7).

What the classifier deliberately leaves in the frame: anything with depth.
The galaxy map's 3D content, the target hologram's model, the loading
screen's spinning ship, the schematic ship render that flight 29 found at
eye line -- all register with NVIDIA already, and the layer would only take
them out of the world they belong in. The classifier is about depthless 2D
UI wherever it is composited, and that is exactly the set that flickers.

## A2. The layer

**One layer per eye, at the FINAL size.** The final size is the size of the
frame the composite will be applied to, which is whatever leaves the door:
under `dlss` the unit-quality size (the pass's `outW x outH` after the
guard's crop), under `dlaa` and `on` the render size after the crop and the
resolve. The openvr half knows it at the end of the door chain and publishes
it for the NEXT frame's draws; a size change costs one frame composited with
a bilinear scale (A4) and re-creates the layer.

**Which eye a draw belongs to.** The d3d11 half must decide at draw time,
which the pass never had to. The key is the pair (render-target resource,
viewport origin). Within a frame the first UI draw's key is the first eye and
the first eye rendered is the LEFT -- the depth probe's convention
(`depth_probe.h:83-92`), verified on 2026-09-03 when the eyes-swapped
candidate lost in three flights. A second distinct key is the right eye. Keys
are remembered across frames by identity, so interleaved eyes (`eye_split.h`
measured the two eyes' draws interleaving, q ordinals 109..151 against
119..181 for one HDR pair) cannot swap them mid-session. A third distinct key
in one frame refuses the frame: stock draws, one log line. G5 says whether
the resource or the viewport origin is the half of the key that varies; the
design carries both so either shape works.

**Format and colour space.** The layer is created in the texture format
family of the game's UI render target, with an RTV of the same VIEW format
the game bound (`ID3D11RenderTargetView::GetDesc` at the first classified
draw). If the game's view is `_SRGB`, hardware blending is linear and the
layer's RTV is `_SRGB` too, so it accumulates in linear light and stores
encoded exactly as the game's target does; the composite then decodes both,
blends, and encodes. If the view is UNORM, everything is in encoded space
and the composite blends raw. This is G1's second half and is settled by the
same census line. 8 bits per channel is enough: the game itself blends the
UI into an 8-bit target (BELIEVED, G1), and a premultiplied 8-bit layer
loses precision only in the dark translucent backing, where the game already
lost it. Do not reach for fp16: 443 MB an eye was the price the light
contract paid, and it was reversed.

**Lifetime.** Cleared to (0, 0, 0, 0) once per eye per frame, at the frame
boundary (`vScreenFrameBoundary`, `vscreen.cpp:3708`, beside
`panelUpscaleFrameEnd`). A frame that classifies no UI draw skips the clear
and the composite both (a black-screen loading phase costs nothing).

## A3. The redirect at the draw

The mechanism is the one every draw-wrapping fix here uses: the verdict
function names the draw, and `Begin`/`End` around `draw()` at
`vscreen.cpp:2791-2844` change state before and restore it after. The layer
is a WRAP applied in addition to whatever verdict the draw has, not a
verdict of its own (A7 says why).

**At Begin**, on the game's own thread, all through the `real*` entry points
so the binding shadow stays truthful (the way `vscreen.cpp:2303-2345` unbinds
around the depth probe):

1. Read and keep: OM render target and depth view (the shadow has both),
   the viewport(s) (`RSGetViewports`), the scissor rects if the rasterizer
   state enables them, the blend state with its factor and sample mask
   (`OMGetBlendState`), the depth-stencil state and reference.
2. Bind the eye's layer as the only render target, NO depth view.
3. Set the depth-stencil state to depth off (a shared EDVR state). The UI
   is depthless anyway (BELIEVED, G2); a family that tests depth gets the
   G2 decision, not a scaled depth buffer.
4. Set the mapped viewport (below), and scale the scissor rects through the
   same map when they are on.
5. Set the converted blend state (below), the game's blend factor and
   sample mask unchanged.

**At End** put everything back, in reverse. Cost: a dozen state calls per UI
draw, thirty to sixty draws an eye. Free when the key is off (one bool).

### The viewport map

Everything the game draws into its render target is placed by its viewport
and its projection. The layer covers the eye's FINAL frustum -- the true
tangents (l, r, t, b) that survive the guard's crop -- while the game's draw
covers the frustum the game was told, (l', r', t', b'), which is the lie's
tangents while the cull guard is live and the truth otherwise
(`system_hook.cpp:601-611`). Both are the openvr half's numbers and it
publishes them per eye per frame. With the game's viewport (vx, vy, vw, vh)
read at the draw and the layer Lw x Lh, a game pixel column X sits at the
tangent

    xt = l' + (X - vx) / vw * (r' - l')

and lands in the layer at

    XL = (xt - l) / (r - l) * Lw

which is affine, XL = ax * X + bx, with

    ax = (Lw / vw) * (r' - l') / (r - l)
    bx = Lw * (l' - l) / (r - l) - ax * vx

Rows count downward from the b edge -- `temporalPixelToDir` in
`temporal_math.h` maps row 0 to `tan[3]` -- so

    ay = (Lh / vh) * (b' - t') / (b - t)
    by = Lh * (b - b') / (b - t) - ay * vy

and the redirected viewport is

    TopLeftX = bx + jx_layer,  Width  = ax * vw
    TopLeftY = by + jy_layer,  Height = ay * vh

with MinDepth/MaxDepth the game's. Fractional TopLeft is fine: D3D11
rasterises with 8 bits of sub-pixel precision, and the bounds are
[-32768, 32767]. Without the guard l' = l and the map is a plain scale and
crop offset; with it, the map reproduces exactly what the guard's crop does
to the game's frame, including to a 2D element the game placed by viewport
fraction, which is the behaviour the player already sees.

### The anti-jitter

The jitter is a shift of all four tangents by (dx, dy) --
`systemHookSetJitter(dx, dy, true)` from `temporalAaFrameBoundary`
(`temporal_aa.cpp:357-358`), computed by `temporalJitterToTangents`
(`temporal_math.h:66-71`). Shifting l and r by dx moves every projected point
LEFT by dx * w / (r' - l') game pixels; shifting t and b by dy moves it DOWN
by dy * h / (b' - t') rows (the comment on that function says so and the
conventions rig of 2026-09-04 confirmed the pixel sign through NGX). In the
layer the same displacement is dx / (r - l) * Lw columns and dy / (b - t) * Lh
rows. To cancel it:

    jx_layer = + dx * Lw / (r - l)
    jy_layer = - dy * Lh / (b - t)

Take (dx, dy) as the tangent shifts, not the pixel jitter (jx, jy): the
tangent form is exact whether or not the game's viewport spans the whole
render width, and it is what the game's projection actually carries.

Which frame's shift: the one live when the draw happens. The openvr boundary
sets the jitter before the game renders the frame, and the draws follow
within it, so "current" is right by the same reasoning the pass uses when it
tells NGX `s.jx`. The F4 read-order instrument found eight projection reads
before the boundary it could not explain (flight 16); if those are the game
reading next frame's projection early, the layer would cancel the wrong
phase and the UI would shuttle by the difference. `advanced.crisp_ui_jitter =
as_is | lag | none` is the A/B: `lag` cancels last frame's shift, `none`
cancels nothing. A family that does NOT carry the jitter (a 2D placement in
NDC, like the intro movie's quad whose position lives in its own vertex
buffer) must be redirected with `none`, or it shuttles the other way; the
default per class is "carries it" (BELIEVED, G4), and the flag is per family
in the direct list.

### The blend conversion

The layer accumulates a PREMULTIPLIED image with coverage in alpha, so that
one composite reproduces what the game's draws would have painted, in order,
whatever their individual blend equations. For each draw the colour equation
is kept and the alpha equation replaced:

| The game's RGB blend (src, dst) | Layer RGB | Layer alpha (src, dst) | Meaning |
|---|---|---|---|
| blending off | ONE, ZERO | ONE, ZERO | opaque write: A = 1 |
| SRC_ALPHA, INV_SRC_ALPHA | as the game's | ONE, INV_SRC_ALPHA | the classic over |
| ONE, INV_SRC_ALPHA | as the game's | ONE, INV_SRC_ALPHA | premultiplied over |
| ONE, ONE | as the game's | ZERO, ONE | additive light: covers nothing |
| SRC_ALPHA, ONE | as the game's | ZERO, ONE | scaled additive: covers nothing |
| anything else | refused | | the draw stays in the game's frame |

All with `BlendOp = ADD`, the game's `RenderTargetWriteMask` kept for RGB
and alpha forced on, `IndependentBlendEnable` off, `AlphaToCoverage` off.
Refused shapes -- subtractive, min/max, `DEST_COLOR` modulation (a
multiplicative tint has no premultiplied form), dual-source, a mask that
excludes alpha's meaning -- leave that draw where it was and say so once per
shape; a refused family swims exactly as before and the totals line counts
it, so the picture never lies about what was treated. Converted states are
cached by the game's blend description (a handful exist; the census's `bl=`
field on the UI draws lists them, G3).

The composite is then, per pixel, in the blend space of the game's view:

    out = layer.rgb + frame.rgb * (1 - layer.a)

which is exact for any sequence of the four accepted shapes by the
associativity of premultiplied over, and is why additive draws must not
touch alpha.

### What the redirect must refuse

- A UI draw whose pixel stage samples something at the GAME's screen scale
  by pixel coordinate. The flight HUD samples the eye-sized depth at PS slot
  0 (`hud_grain.cpp:148-155`); if its UV is derived from SV_Position and a
  screen-size constant, a draw rasterised at another scale reads depth from
  the wrong place, and its cockpit fade breaks. If it is derived from the
  projected position (NDC), it is scale-free and fine. Read ps
  `8DEF46452FA459F5` before shipping that family (G7); the refusal is per
  family, and a refused family stays in the frame.
- Draws through a deferred context or a command list: the shadow does not
  see them (no `ExecuteCommandList` was ever observed in the cockpit, flight
  6), so nothing to do, but the classifier must not assume the immediate
  context.
- Any fault inside Begin/End: the `FaultBudget` pattern, stand down for the
  session, forward stock, one line.

## A4. The composite at the door

A new door step, LAST on every forwarding path in `hookedSubmit`
(`compositor_hook.cpp:1113` onward), after `applySharpen`, with the same
discipline as the resolve and the sharpen: every path once, or none. Placing
it after RCAS keeps the sharpener off the text; the layer is already at its
rasterised crispness and RCAS on thin strokes rings. The step takes the
texture and bounds a path is about to forward, and follows `sharpen_pass.h`'s
contract exactly: an EDVR-owned per-eye texture, region-sized, in the
source's format family, full-span bounds out, null means "forward what you
had" with a line saying why.

The pass: one compute dispatch per eye over the region, loading the frame and
the layer at the same pixel, applying the equation above in the right space
(A2). When the frame's region size differs from the layer's (the frame of a
size change, or the resolve engaging), sample the layer bilinearly at the
matching fraction and re-arm the layer at the new size; one soft frame beats
a missing UI. When the layer has nothing this frame (no UI draw classified)
the step returns null and costs nothing.

Non-forward paths -- the FSS theater's own rendering, the heal's composite,
the withhold's repeated frame -- do not run the temporal pass either, and the
UI layer follows the temporal pass's placement exactly (the comment at
`compositor_hook.cpp:1178-1184`). The FSS draws its own UI through the same
GUI families, so the redirect must stand DOWN while the theater or the heal
is engaged, or the FSS loses its labels. The openvr half publishes "armed"
per frame along with the sizes, and the d3d11 half redirects nothing while it
is false. A frame that was redirected and then took another path shows
without its UI, once; the log counts those.

## A5. The surfaces at the target resolution, and the CAS question

Design A draws the UI's composites at the output size, but each composite
samples a surface the game rasterised at the render size. At DLSS Quality
that is a 1.5x bilinear magnification of two-thirds-resolution text: stable,
unjittered, and soft. The layer fixes the swim; the surface's own resolution
fixes the softness, and there are two ways to raise it.

**Inflate the surface (preferred, with one thing to prove first).**
`advanced.surface_inflate` already creates a named surface N times larger
at `CreateTexture2D` and scales the game's viewport to match
(`fss_res.cpp:191-224`; the `RSSetViewports` path plus the draw-time
backstop at `vscreen.cpp:1612-1630`), and the target-indicator arc verified
the mechanism engaging in the field: six textures created, six viewports
scaled, no failures, frame rate unchanged (2026-09-02). Vector geometry
provably sharpens with a bigger surface -- the widget shader draws in a
normalised space and places each element by a matrix (`loader_panel.h`),
so more pixels is more detail. TEXT is the open question. Both inflate
flights were judged on the fixed-size target indicator alone and recorded
"nothing visible changed"; nobody looked at the letters. The text shader
samples a 2048x2048 A8 glyph atlas (the loading-panel census,
`docs/loading-panel-handoff.md`), and whether that atlas is baked for the
resolution the game started at, or holds glyphs large enough (or as distance
fields) to survive magnification, is unmeasured. The natural experiment that
sharpened the HUD's text -- a 1.635x larger eye between headsets -- let the
game bake for the new size; inflation happens behind the game's back. Hence
G9 before this ships: one flight with the cockpit surfaces inflated at HMD
Quality 0.67, after a menu trip, judged on the text; and a look at whether
the 2048x2048 A8 texture is created again after a resolution change (the
create hook sees every texture; a fresh bake means the atlas follows the
size, and a single creation per session means it caps the text).

Three extensions, the first a requirement rather than a convenience:

- **A float factor, exactly output over render.** The composite samples
  the surface with the game's own sampler -- plain trilinear (the 2026-09-04
  census: 552 of 613 samplers) over a single-mip target. A 2x surface under
  DLSS Quality is 1.33x denser than the layer's pixels: minification with no
  mip chain, which sparkles on thin strokes as the head moves -- the exact
  class of artefact this design exists to remove, and now with no temporal
  filter over the layer to hide it. A factor of 1/0.67 = 1.5 at Quality
  (1.0 under DLAA, where the mode does nothing) recreates each surface at
  the size the game would have made at HMD Quality 1.0, so the
  surface-to-panel ratio is the one the game designs for, and the fixed-size
  sprite comes out exactly as it does at 1.0 -- no better, no worse. Today
  the parser refuses anything but integers 2..4 (`fss_res.cpp:114`,
  `:167-171`) and the factor is a `uint32_t` (`fssResScaleOf`); it becomes a
  float, and the viewport paths already multiply rather than double.
- **A `match` mode, with the sizes learned rather than named.** Every UI
  surface the classifier learned is a size to inflate at its NEXT creation.
  That arc's lesson is why: surface sizes are valid only for the session
  they were measured in, and its "odd size with a depth partner" rule
  filtered out the one surface that mattered. The cockpit's surfaces are
  created at session start and recreated on a trip through the main menu or
  a resolution change, so the first session after enabling it inflates
  nothing until the menu is visited; the totals line says "N surfaces
  learned, M inflated, the rest at their next creation."
- **The tracked ring.** `fss_res` keeps 32 entries shared with the FSS rule
  and evicts entries whose textures are still bound (`fss_res.h:24-31`).
  Every learned surface and its depth partner is two entries; size the ring
  for the learned set, or evict by liveness.

Two limits, both measured in the hud-surface arc: artwork laid out at a
FIXED PIXEL SIZE inside a surface (the target direction indicator) does not
sharpen however big the surface is, and line widths held in surface pixels
would thin (open risk, never observed). Depth partners are matched by size
and inflate alike.

**If G9 fails, the lever beyond inflation is the game's own layout size.**
The on-foot screen's resolution is already forced by rewriting the
immediates at six call sites where the game decides it (`vscreen_res.h`),
which makes the game lay out AND bake for the size it is told -- inflation
cannot do the second half. The cockpit surfaces are a fixed fraction of the
internal render size; the arithmetic that applies that fraction is the
analogous patch, and a bigger layout would bake the atlas and place the
sprites for it. The target-indicator arc judged that out of proportion for
one sprite; for every piece of UI text under DLSS the sum is different. Not
in this plan: the fallback if inflation cannot reach the letters.

**Resample the surface (Sean's CAS).** `experimental.holo_panels = sharp`
(`panel_upscale.cpp`) does exactly this for one panel: AMD's EASU then RCAS
over the interface surface into an EDVR texture, bound into slot 2 for the
matched draws. Generalising it to every learned surface is a small change.
But it is a spatial upscale of a two-thirds rasterisation: it cleans edges
and adds no detail (the file's own header: "No resampler invents detail"),
it costs a full resample per surface per frame (19.5 Mpx of EASU for one
panel at 2x), and inflation gets the same pixels rasterised properly for the
price of the GUI's own draws. Keep it as the fallback for a surface the game
never recreates in a session, and for the fixed-size sprite case where
nothing else applies. Do not build it first.

**Direct UI has no surface.** The flight HUD's vectors and the target
indicator's quad are rasterised into the eye; under design A the redirect
rasterises them at the output size natively and nothing else is needed.
Under design B they stay at the render size, and no CAS reaches them.

## A6. Keys, log lines, debug views, stand-down

Keys follow the house rule that values say what the player gets.

    [fix]
    # Menus, HUD and loading text drawn at the headset's resolution, outside
    # the temporal anti-aliasing, so they stay crisp and still under DLSS
    # and DLAA. Needs temporal_aa on; does nothing otherwise. Live.
    # ui: Crisp menus and HUD under DLSS | choices on, off
    crisp_ui = off

    [advanced]
    # crisp_ui_surfaces = stock | match   (inflate learned UI surfaces to the output size)
    # crisp_ui_families =                 (extra vertex-shader hashes treated as direct UI)
    # crisp_ui_exclude =                  (hashes never redirected)
    # crisp_ui_jitter = as_is | lag | none
    # crisp_ui_debug = off | layer | coverage

`crisp_ui_debug = layer` submits the layer alone over black -- the single
most useful flight instrument: everything the classifier caught, crisp, in
the eye it was assigned to; anything missing is an unclassified family,
anything extra a false positive, a swapped eye is instant double vision.
`coverage` paints the layer's alpha.

Log lines, in the graphics log:

- engage: "crisp ui: engaged -- layer WxH per eye, FORMAT (VIEW), composite
  in linear|encoded space, eye keys by resource|viewport";
- every 20 s: "crisp ui: N draws an eye redirected (C composites, D direct,
  R refused by blend shape, F families), composite X ms, surfaces learned S
  inflated I, frames without composite W";
- once per shape: the refusals, with the blend description;
- stand-down: why, once.

Stand-down rules: no temporal mode on; the runtime not DirectX; any fault
budget exhausted; a third eye key; the layer's create failing. Every one
forwards stock and says so. The key is live; turning it off mid-session
composites the frame in flight and stops redirecting at the next boundary.

## A7. Interactions

- **Verdict composition.** The layer wraps the draw regardless of its
  verdict, so `panelUpscaleBegin` (slot 2 swap), `hudGrainBegin` (slot 1
  swap) and the on-foot `panelCurveSubstitute` (geometry swap, which draws
  its own geometry and swallows the game's) all compose: Begin the layer
  before the verdict's Begin and before the substitution, End after. The
  only verdicts it must skip are the withholds.
- **The cull guard.** Handled by the tangent map. A restage changes the
  render size and the eye targets: the classifier's sets evict, the layer
  re-creates, one frame composites scaled.
- **`temporal_aa = on`, `dlaa`, `dlss`.** All three benefit identically; the
  layer is agnostic to whose history runs. Off: stand down.
- **The supersample resolve.** When the pass is off and only the resolve
  runs, stand down (the UI is already full-resolution and unjittered, and
  the resolve's calm kernel is the player's choice). When both run, the
  final size is the resolve's output and the map handles it.
- **`fix.render_sharpness`.** Before the composite by construction.
- **`experimental.holo_panels = sharp`.** Composes; redundant once
  `crisp_ui_surfaces = match` inflates the same surface.
- **The foveation branch** (`claude/foveation-gaze-probe-2ff1267`,
  unmerged). Independent: the composite is the last step, after the
  fovea/periphery join.
- **The intro movie and the splash.** Their composites sample a video plane
  and a still, not a GUI surface, so they stay in the frame; the loader's
  dialog goes through a surface and moves to the layer. `splash_dim`'s
  re-issue of the backdrop composites is unaffected.
- **The on-foot screen.** Its composite samples a surface the GUI drew
  (BELIEVED), so it classifies as UI and the 2D screen is composited crisp
  after DLSS -- desirable, and the first flight on foot must confirm
  `panel_distance` and `panel_curvature` still place it.
- **The wake pulse, the loader panel, the scrim.** All act on draws INTO
  surfaces or on withholds; none is a composite; unaffected.

## A8. Cost

| Item | Price |
|---|---|
| Layer memory | Lw x Lh x 4 per eye: 74 MB at 4340x4284, 59 MB at 3816x4080 |
| Clear | one per eye per frame, tens of microseconds |
| Redirected draws | 30-60 an eye, each a few quads: noise |
| Composite | one full-region dispatch per eye; RCAS measured 0.23 ms an eye at 5792x5356, this kernel is simpler |
| Surface inflation | (1/scale)^2 on ten panel-sized surfaces: small next to an eye |
| Classifier | a hash lookup per offscreen draw while on, pointer compares per eye draw |

# The gates: measure before building

One session, three censuses: docked in the cockpit, at the main menu, and
during a loading screen (`advanced.census_at_ms` for the last; `edvr.ini:996`
explains the schedule). `advanced.census_offscreen = 1` and
`advanced.census_frames = 3`, then the census key. Read the `DC` draw lines
(`draw_census.cpp:913-917`): `vh=` is the vertex shader hash, `ph=` the
pixel shader's, `r=` and `d=` the render and depth view tokens, `s=` the
first four pixel-stage SRVs, the state tail `ds=` (depth enable, func, write
mask), `bl=` (the whole slot-0 blend equation), `bm=` (write mask); the
interned table at the end (`DC id @N tex WxH fmt=F res=P`) gives each
token's size and DXGI format; `DCO` lines are the offscreen draws; `DCC`
lines are the copies.

| Gate | Question | Read | If it says otherwise |
|---|---|---|---|
| G1 | Are the composites drawn into the tonemapped 8-bit target, and through an sRGB or a UNORM view? | `r=` on the `81216C77F90DEDD6` lines, then that token's `DC id` line: `fmt=` is the texture's format and `vf=` the view's (added 2026-09-06 for this gate; for RGBA8, 27 typeless, 28 UNORM, 29 UNORM_SRGB; 10 = RGBA16F, 26 = R11G11B10F) | HDR target: the layer bypasses tonemap and bloom and the HUD's look changes -- flight it with `layer` view beside stock, or go to design B |
| G2 | Do the UI draws bind a depth view or test depth? | `d=` and `ds=` on the same lines | a family that tests depth: decide per family between ignoring the test (panels win) and leaving it in the frame |
| G3 | Which blend shapes do the UI families use? | `bl=` | a shape outside the table: extend the table if it has a premultiplied form, else that family stays |
| G4 | Do the composites carry the eye's projection (the jitter)? | flight A/B: `crisp_ui_jitter = none` makes a jittered family shuttle; `as_is` makes an unjittered one shuttle | per-family jitter flag in the direct list |
| G5 | Per-eye render targets, or one double-wide with viewports? And is the eye target copied into the submitted texture? | `r=` tokens across the two eyes' UI draws, the `DCC` lines' `dst=` | the key handles both; only the log wording changes |
| G6 | Does a GUI family ever draw straight into a submitted eye target? | GUI `vh=` on `DC` (not `DCO`) lines with an eye `r=` | if yes, those draws are direct UI with `crisp_ui_jitter = none` |
| G7 | Does the flight HUD's pixel shader derive its depth UV from SV_Position and a screen size? | disassemble ps `8DEF46452FA459F5` (the census's `ph=`), look for `vPos`-based UVs | refuse the family, or bind a depth resampled to the layer's size at slot 0 for it |
| G8 | Does any pass read the UI's target after the UI draws, other than the copy to the submitted texture? | draws after the last UI draw with the UI target in `s=` | an in-game AA pass over the HUD means the stock HUD has edge AA the layer lacks: a 2x layer with a box downsample restores it; with DLSS on, the game's AA should be off anyway |
| G9 | Does inflating a surface sharpen its TEXT, or only its vector lines? | one flight, no build: `surface_inflate` on the cockpit surfaces (2x today, `match` once it exists) at HMD Quality 0.67, after a menu trip, judged on the letters; and whether the create hook sees a fresh 2048x2048 A8 texture after a resolution change | text unchanged: the glyph atlas caps it -- the layout-size patch (A5) or vector-only gains; design A still removes the swim either way |
| G10 | After the last UI draw, does anything test against or read the scene depth the UI draws bind? | `tools/crisp_ui_gates.py`, the G10 section, on a census with identity tokens (3 frames, the 2048-entry table) | a later depth-tested draw or a depth read: B0 would change the game's picture -- use B with the copy |

G1 and G3 decide whether A can ship as designed; G9 decides whether A5's
inflation is worth carrying or the layout-size patch is the next arc; G5 and
G6 only choose branches the design already has; G2, G4, G7 and G8 are
per-family and can be settled after the first flight with the `layer` view.

# The test plan

**Desk, in `tools/smoke` (a real device, the existing harness):**

1. The map and the anti-jitter: draw a one-pixel cross at a known tangent
   through a jittered projection, all eight Halton phases, with and without a
   guard lie, through the redirect's viewport; read the layer back; the cross
   must land on the same pixel with the same coverage every phase. A wrong
   sign shows as a one-pixel shuttle across the phases. Keep the conventions
   rig (`ccbcf43`) as the reference for the pixel sign.
2. The blend conversion: the four accepted shapes, overlapping, drawn once
   directly into a frame and once into the layer then composited; max
   absolute error 1/255 per channel, in both colour-space cases.
3. Eye keys: two interleaved targets, then a third; the third refuses.
4. The composite at a mismatched size: a soft frame, not a missing one.

New `edvr*` test exports go on `build.bat`'s `--extra-export` list, or
`GetProcAddress` returns null (2026-09-05's lesson). Verify a build by the
DLL's timestamp and an error-line count, not the exit code.

**Flight (Pimax, native SteamVR, `temporal_aa = dlss`, HMD Quality 0.67, the
Steam install; the log's version line must name the build):**

1. `crisp_ui_debug = layer`, docked: every cockpit panel, the flight HUD
   and the target indicator, crisp, each in its own eye. Note what is missing
   and what is extra.
2. `debug = off`: text at rest, under a slow head turn, under a fast one;
   the world through the panels; then in space near a station, ship turns.
   The registration probes do not apply to the trained path; the eye is the
   instrument here.
3. The main menu and a loading screen: the per-class counts on the totals
   line, and the `layer` view again.
4. On foot: the 2D screen through `panel_distance` and `panel_curvature`.
5. `crisp_ui_jitter = none` for G4.
6. After a menu trip with `crisp_ui_surfaces = match`: "S learned, I
   inflated" on the totals line, and the text by eye against `stock`.
7. `crisp_ui = off` live, and back on: no stand-down, no missing frame.

Read afterwards: the engage line, the totals line, the refusal lines, and
"frames without composite".

# Risks

- **The composites are in the HDR target** (G1). The layer would then skip
  tonemap and bloom and the HUD's colour and glow change. The RemLok overlay
  precedent says post-tonemap; the holo panels are unmeasured.
- **Refused blend shapes** leave families swimming; the totals line names
  them, the picture shows it.
- **The jitter phase** (the eight unexplained early reads). The `lag`
  instrument is the fix if it bites.
- **Eye keys swapped** by a frame whose first UI draw is the right eye's.
  Keyed by identity after the first frame; the `layer` view catches it in a
  second.
- **Families the classifier misses** in contexts nobody censused (the galaxy
  map, station services, the SRV). The fingerprint is the GUI renderer, so a
  miss means a UI drawn by something else; the census names it, the direct
  list takes it.
- **A first session inflates nothing** until the surfaces are recreated;
  the log says so.
- **Memory** on small cards: two 8-bit layers is under 150 MB at the largest
  headset; print the figure on the engage line.
- **The FSS's UI** while the theater runs: stand down, above.
- **In-game AA on the HUD** (G8).

# Design B: the depth layer, the fallback

## B1. Mechanism

The same classifier names the same draws. Instead of redirecting them, END
re-issues each one -- same vertex and pixel shaders, same input assembly,
constants and textures, same viewport -- with the output merger changed to
NO colour target and an EDVR-owned depth target of the game's render size,
and a depth-stencil state of depth enabled, `ALWAYS`, write all. The game's
pixel shader keeps running, so where it discards on alpha the depth lands
only on visible strokes; where it does not, the whole quad stamps its
distance, which is the right answer for a panel with a backing. The target
is cleared to 0 (the far plane under reversed-Z, the scene's own convention)
at the frame boundary. The re-issue is the pattern `splashDimBegin` uses
(`vscreen.cpp:2820-2824`): the state the draw needs is still bound.

The pass reads that target as a layer beside the scene's depth, per eye,
paired by the same eye key as A2: `zAt(q)` becomes "the layer's value where
it has one, else the scene's", in the fetch loop, the mv loop, and NGX's
depth copy. UI pixels then carry the head's full rotation-plus-translation
delta at the panel's true distance, which is the registration the cockpit
already gets -- and flight 30 recorded that "THE COCKPIT AND THE SHIP'S
EXTERIOR SHOW NO GHOSTING AT ALL under movement". NVIDIA then accumulates
the text instead of rejecting it.

## B2. What to restore from git

The read path existed and was removed in the cleanup `df68d8d`: `Z2`/`Z3` at
t3/t4, `zLayerAt`, `zAt` preferring the layer, the two 3x3 dilations kept as
separate maxima (`zs`/`zl`, preferring `zl`), the bright-with-layer counters
28/29, the `depth` debug view painting layer pixels green. `git show
9e20ea4:src/d3d11/temporal_pass.cpp` has all of it. Do NOT restore
`depthProbeLayerDepths` or `temporal_aa_depth_layers`: that machinery went
looking for layers the game might have written, found only the ship's
schematic render, and was rightly retired. Here EDVR owns the layer and
knows which eye it is, so the pairing is a lookup, not a census.

## B3. The encoding gate

The layer receives the UI draw's own `SV_Position.z`, encoded by whatever
projection that draw carries. The scene decodes with the smallest-near pair
the receiver captured (0.025 / 50000, `temporalDepthToMetres`); the game also
asks for 0.1 / 1000 and 1 / 50000 (flight 10). If the UI composites project
through a different pair, the layer decodes four times too far -- flight 9's
exact fault (5 m read for 1.25 m). Gate: docked, `temporal_aa_debug = depth`
and the 4x4 nearest-depth map on the sample line must put the panels at
0.6-2 m (flight 11's map: dashboard 0.3-0.7, panels 1-2). If they read
metres too far, the layer takes the second pair's near/far (the receiver
logs each pair once; publish both). The cleaner alternative is an EDVR pixel
shader per family writing linear view depth in metres into an `R32_FLOAT`
target, which needs each family's vertex output signature transcribed
(`target_sharp` did one); take it only if the pair question refuses to
settle.

## B4. Collateral and limits

- Where a panel is see-through, the world behind it takes the panel's
  motion and smears under a SHIP turn (head turns are fine: rotation is
  depth-free). The backing is dark and most panels sit over the dashboard,
  so this should be minor; a star field seen through a panel's glass is the
  case to look at.
- The text is still rasterised at the render size and reconstructed; with
  `crisp_ui_surfaces = match` NVIDIA integrates a 1.5x-denser texture and
  the result is good, but it is DLAA-quality text, not native. Direct UI
  (the flight HUD's vectors) stays at the render size with no remedy.
- Menus and loading screens get a depth where they had none, which also
  ends "the menu wall moves with my head" for the panel itself (the hangar
  behind it is the world path's business, and `menu_metres` stays 0).
- Twice the UI draws, which is nothing.

## B5. When to choose B

B is a third of A's code and changes nothing about how the game paints.
Choose it when G1 says the composites are in the HDR chain and the look
matters more than native text; when G3 refuses too many families; or as a
first flight to prove the classifier before A's composite exists (the
classifier and the eye keys are shared, so nothing built for B is wasted).
B and A can also coexist: a family A refuses can still be re-issued into
B's layer, so every classified draw gets one treatment or the other.

# Do not

- **No luma heuristics for UI depth.** `temporal_aa_hud_metres` reprojected
  every bright distant thing at 2.4 m and painted heat haze on stations
  (2026-09-04, evening). The classifier is what replaces it.
- **Do not leave the UI in the DLSS input unjittered.** NGX un-jitters the
  whole frame; an unjittered element then shuttles by the jitter instead.
- **Do not write UI depth into the game's own scene depth without G10.**
  Anything the game reads from that buffer after the UI would see panels
  in it. In the cockpit G10 measured nothing reading it (2026-09-06), which
  is what makes B0 admissible there; the menus and the loading screen need
  their own G10 before B0 is trusted in them, and B's separate copy is the
  form that never needs the question answered.
- **The bias-current-colour mask is an instrument, not the fix.** The SDK
  exposes it (`nvsdk_ngx_helpers.h:152`, unwired in `dlaa.cpp:361-365`); a
  coverage mask from the same re-issue would stop the swim but leave the UI
  a spatial upscale of a jittered two-thirds frame -- shimmer, not crisp.
  Twenty lines once the coverage exists; worth one A/B, not a release.
- **MSAA for the HUD** stays declined.
- **Do not trust a surface size across sessions**, an index count across
  builds, or "the only draw the A/B added" without a suppression test
  (`edvr-cockpit-hud-workstream`'s lessons).

# File-level plan

New, mirroring the sharpen's split (the openvr half decides, the d3d11 half
does):

- `src/d3d11/crisp_ui.h/.cpp`: the classifier (surface set, eye-target set,
  composite and direct rules, eye keys), the layer per eye, `crispUiBegin/
  End` for the wrap, the frame-boundary clear, the composite dispatch, the
  exports `edvrCrispUiFrame(eye, trueTan[4], drawTan[4], layerW, layerH,
  jitDx, jitDy, armed)` and `edvrCrispUiComposite(srcTex, eye, bounds)` (the
  `edvrSharpen` shape), plus `edvrCrispUiCounts` for the harness.
- `src/openvr/crisp_ui.h/.cpp`: the key, the per-frame publish from the
  boundary (tangents from `systemHookEffectiveTangents` and the true set,
  the jitter as set, the final size from last frame's door), the door step
  `applyCrispUi` last on every forwarding path, the stand-down voice.

Touched:

- `src/d3d11/vscreen.cpp`: the offscreen branch learns surfaces (one hash
  lookup); the wrap around `draw()` at `:2812` outside the verdict's
  Begin/End and the curve substitution; the boundary clear beside
  `panelUpscaleFrameEnd`; the eye-target set fed from the copy hooks.
- `src/openvr/compositor_hook.cpp`: `applyCrispUi` after `applySharpen` on
  each path.
- `src/openvr/temporal_aa.cpp`: nothing if `crisp_ui.cpp` reads
  `systemHook*` itself; otherwise the boundary publishes.
- `src/d3d11/fss_res.h/.cpp`: float factor, `match` mode, the learned-size
  matcher.
- `src/d3d11/draw_census.cpp`: the RTV view format on `DC` lines (G1).
- `edvr.ini`, the config contract, `tools/gen_settings_schema.py` (the
  `ui:` line only), `build.bat` (`--extra-export`), `tools/smoke/smoke.cpp`
  (the four desk tests), `docs/anti-aliasing.md` (the decline paragraph
  points here; "The order at the door" gains the last step), README.

For B: `src/d3d11/temporal_pass.cpp` (the restored read path, the `depth`
view's green), `crisp_ui.cpp` (the re-issue and the depth target).

# Open questions for Sean

1. The key's name. `crisp_ui` says what the player gets; `hud_detail =
   stock | full` is the other candidate.
2. Default off for the first release, on once flown on both headsets?
3. Should the tester handout recommend the game's own AA off with DLSS
   (G8's cheap answer)?
4. Is 150 MB of layer acceptable on the smallest card a tester has, or
   should the layer refuse below a VRAM floor?
5. Whether the CAS resample path (A5, second option) is worth carrying at
   all, given inflation.
