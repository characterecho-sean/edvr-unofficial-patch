# Five elements that look indistinct under DLSS (2026-10-14)

## Status

- **State (2026-10-14):** triage done, no element root-caused, no fix tried. The log-only blend probe is BUILT, NOT FLOWN, on
  `claude/blend-probe` (cut from `claude/flight-2026-10-14`, 57f5393e). Its rig is `tools\blend_probe_test`, in the build gate.
- **The five elements:** (1) the hologram projected over the landing pad in the station (the user confirmed: not the cockpit docking
  display); (2) the gantry railing while the pad moves; (3) the hologram adverts; (4) weapons fire; (5) the station "mail slot" force field.
- **Environment:** DLSS preset K (`fix.temporal_aa_model = k`), 2016x2016 input to 4032x4032 output per eye (HMD Quality 0.50), Elite
  supersampling held at 1.0. The 10-13 21:43 log names the runtime `steamvr-openxr/steamvr-openxr-aapvr`, not Pimax OpenXR.
- **Finding 1 (PROVEN):** the bias-current-colour mask (the old `ui_depth_reactive = 0.5` setting, baked in by the 2026-10-09 key cull) does nothing on preset K (dlaa.cpp:711, "only preset F supports this input"). K's only
  anti-ghost path is EDVR's post-DLSS UI resolve, which reads the coverage mask, and only the HUD, cockpit hologram and smoke families set it.
- **Finding 2 (PROVEN):** no world-space transparent or additive family is in any coverage list except thruster smoke (`5E417E9D` /
  `BD801F2F`) and the target reticle (`71DD8B8B`). The old `temporal_aa_hologram_families` extras (also culled) joined the cockpit list only, so no user-reachable way to add a world family existed.
- **Finding 3 (PROVEN):** engine-record motion for movers is verified for settlements and landing ships only. The ini says stations and
  ships in space are not verified.
- **Mechanism (PROVEN for cockpit holograms, INFERRED here):** a pixel that writes no depth takes the depth, and so the motion vector, of
  what is behind it. A head move makes that wrong for anything nearer than its background; at a 2x upscale a thin line is under one input
  pixel wide, so DLSS leans on its history.
- **Grouping:** G1 = elements 1, 3, 4, 5 (translucent or additive world effects with no depth or motion of their own). G2 = element 2 (an
  opaque mover whose motion is probably the camera term alone). Elements 1 and 3 may share a hologram material.
- **Candidates:** element 1 may be the additive six-vertex sprite pair VS `94D5C556DFD6D705` / PS `912477AEF6958379`, which the glare fix
  and the UI layer both claim (log 10-13 21:43 and 21:55). NOT proven for the pad. The railing's pair is not known to be in `kFamilies`.
  The adverts, the weapons and the force field have no hash in any doc.
- **Temporary key:** `hotkey.blend_probe` (section `[hotkey]`, unbound; use a free key, for example F3 (avoid keys the game uses)). Remove it, `src\d3d11\blend_probe*`,
  `tools\blend_probe_test`, the four small hooks in vscreen.cpp / temporal_pass.cpp / ui_depth / ui_layer, and its build.bat lines when
  this arc closes (ship the winning behaviour, propose removing the key, quoted, as AGENTS.md Scope control says).
- **Cannot be captured on this build:** `advanced.temporal_aa_debug` (the `motion_source`, `ui_layer`, `depth`, `motion` views) and
  `advanced.temporal_aa_diagnostics` were removed by the 2026-10-09 key cull (53e6f047), on main too. The triage's zero-build views do
  not exist. The grid's `p` (motion path) and `e` (engine kind) columns answer the same railing question per cell. Bringing a view back
  would be a separate change; it is not folded in here.
- **Confound:** AA off may not render the same pixels as DLSS. Check the AA-off render size before reading "worse than AA off" as a bug.
- **Flight plan:** section "Flight plan" below. Set `fix.particle_billboard = stock` first.
- **Next flight:** one session, five elements, two presses each (still, then head moving), the railing across a pad raise and lower. Read
  the log with `python tools\edvr_log.py --target steam --expect-build HEAD`, then grep `blend probe`.
- **Ruled out:** the 2026-09-29 pad-into-layer rule as wired in 9122f31e (docs/openxr-performance-review-2026-09-14.md:1660); reactive
  bias or sharpening as a fix (docs/hologram-depth-2026-09-24.md "Ruled out"). Do not propose them again.

## Triage

Read-only, against origin/main 7e44b86c and `claude/flight-2026-10-14`. Source cites are line numbers in those trees.

What EDVR gives DLSS today:

- **The bias-current-colour mask** is handed over at dlaa.cpp:704-711, built in temporal_pass.cpp from the UI coverage mask plus
  engine-masked pixels. Preset K ignores it, so the reactive setting is inert on K.
- **What marks it** (ui_depth.cpp, `g_reissueMaskSlot`): the interface composites, the holo material's markers, the flight HUD strokes.
  Selection is by shader hash after the draw ladder in vscreen.cpp, not by blend state. There is no generic blend-based selection.
- **Smoke** is the only world transparent family with its own treatment: a coverage replica writes depth where alpha is at least 0.08
  (`kSmokeFloor`), so the dense core has its own depth and motion. Each new family needs its own pixel-shader replica.
- **The generic hologram pass** takes ten cockpit families (radius-clipped to 10 m) and one world marker, the reticle.
- **The post-DLSS UI resolve** (ui_resolve.h) is the real reactive path for K; **the UI layer** (`fix.ui_quality`) draws admitted families
  after the upscale and already admits `94D5C556DFD6D705`.
- **Engine-record velocity** (`engine_velocity_families.h` `kFamilies`) gives exact motion to keyed pairs with certified previous poses.
  Weapon motion is the on-foot first-person weapon only, not ship weapon fire.

| # | Element | Category (INFERRED) | Test | Fix route if confirmed |
|---|---|---|---|---|
| 1 | Pad hologram over the pad | translucent or additive, no depth; maybe the shared sprite pair | does the draw reach the window; routed to the layer or left in the frame; depth and motion of its pixels | per-draw discriminator (instance shape or range), never the hash alone: the pair is shared with the glare, radar contacts and the hangar sprite |
| 2 | Gantry railing in motion | opaque mover on the camera term | grid `p` = 11 and `e` = 1 on the rail, or `p` = 1/2 | key the railing's VS/PS in `kFamilies` and confirm the record certifies; touches the optimized-station veto, and a bad key caused the 2026-09-29 plating crawl |
| 3 | Hologram adverts | animated texture on a world quad | same as 1, still versus moving | shared with 1 and 5 once the hash is known; may share `81216C77` with the cockpit material |
| 4 | Weapons fire | thin fast additive geometry, no velocity source | row present while firing at a dark target | per-family coverage or layer route; no per-projectile velocity exists |
| 5 | Mail slot force field | scrolling additive texture, like the drive smoke | row present; grid depth equal to the far background's | the smoke recipe (coverage replica, depth floor, mask) or the layer route |

Order: one instrument flight, then G1 starting with 1 and 3, then 5, then 4, then G2. Not recommended: a global transparent mask (K ignores
it), sharpening or contrast tweaks.

## The instrument: `hotkey.blend_probe`

Log only. It changes no draw, no persistent state and no pixel. Unarmed, each eye draw pays one relaxed atomic load, the temporal pass one
more per frame. Press it with the element at the centre of the view.

A press arms a capture; the next frame boundary starts 3 consecutive frames; the answers are read back over the following frames
(up to 120 boundaries) and the report is written.

**Rows.** An eye draw is a candidate when it writes colour and is blend-enabled or writes no depth. Eye 0's draws are tested (eye 1 is
counted and skipped; `eye=-1` means the pass's eye table could not tell, and then both eyes' draws appear). A candidate is tested by a
scissored duplicate of the draw (the look probe's method, from `claude/hangar-test-4` look_probe.cpp) that writes no colour, depth or
stencil, bracketed by an occlusion query; its `cov=` is the samples that pass the draw's own depth test and survive its shader, inside the
96x96 window at the viewport centre. A candidate with `cov=0` is not listed. Draws with a UAV or stream-output target cannot be duplicated
and are listed with `cov=?` and counted; indirect draws are counted, never tested. At most 200 rows a frame are listed, the last 200 in
issue order (later draws land on top); the rest are `truncated`.

Row columns (`blend probe row:`):

- `f` captured frame 0..2; `gf` the game frame; `#` the draw's place among the frame's eye draws; `eye`.
- `vs` `ps` the 16-hex shader hashes; then the draw kind (D, I, N, X), `n` vertex or index count, `inst` instances.
- `blend=[src,dst,op/srcA,dstA,opA wm=mask]` (or `off`), `depth=[test FUNC write 0|1]`, `rt=` format and size.
- `cov=` covered samples in the window, `?` when unmeasured.
- `verdict=` EDVR's draw verdict (`none`, `skip`, `particle`, `glare-steady`, ...).
- `ui=FAMILY/DECISION`: the UI layer's family for the draw (`-` if none) and what it decided: `taken`, `left:REASON`, or `none` when no
  decision ran. `left:redirect` is the VR world route re-issuing a draw it did not take.
- `holo=cockpit|world|-`: the generic hologram pass's list that matched.
- `mask=SLOT:mF/A`: the ui-depth coverage family reissued this draw into private depth and marked the reactive mask; F and A are the
  strength in 1/255 quanta for a floating and an attached stroke (ui_depth.cpp `floorBuffer`: the holo material and the flight HUD write 1
  and 2, the others 1). `SLOT:nomask` is coverage without the mask; `-` is neither.
- `kf=`: the pair's place in `kFamilies`: `rigid#N`, `skinned#N` (N the family index), `self-marking`, `vs-only#N` (the vertex shader is
  keyed, this pixel shader is not), or `-`. `eng=sub|plain`: the pool pixel shaders were substituted for this draw.

**Grid.** The temporal pass stages a 96x96 crop of eye 0's decision texture (physical motion, decision flags), depth copy, coverage mask
and bias mask after the motion dispatch, using its existing capture-only decision shader variant (the production motion output is
unchanged). The crop is read back at the next frame boundaries and sampled at 8x8 cell centres (pixels 6, 18 ... 90). Lines:
`blend probe grid legend:` (once), `blend probe grid f=N:` (window, the pass's camera term: delta rotation in degrees and translation,
which inputs were staged), eight `blend probe grid f=N row J:` lines of `[z mv p e c b]` cells, and `blend probe grid f=N summary:`.

The summary is the railing answer: the median motion over all cells, over camera-path cells (`p` 1 or 2) and over engine-path cells (`p`
11), the difference of the last two, and a histogram of engine kinds. A moving railing with `p` = 1/2 is on the camera term alone. One
with `p` = 11 and `e` = 1 has exact per-object motion. The pass's camera term is reported as rotation and translation rather than a
per-pixel value because the decision texture has no room for it (a second capture shader variant would be a bigger change).

Other lines: at install `blend probe: off; hotkey.blend_probe is bound to KEY` or `... is NOT bound -- add "blend_probe = <a free key>" under [hotkey] in edvr.ini to use it` (check it before flying); `armed (trigger ...)`; `collecting 3 frames from game frame N`; per frame
`blend probe: frame f=N (game frame G): E eye draws seen (...); C candidates ..., T tested, K cover the window, L rows listed, truncated X;
not tested: ...; M answers missing`; `blend probe: captured 3 frames, R rows (G of 3 motion grids staged)`. Refusals read
`blend probe: refused (REASON)`: not VR, a capture already running, no eye pass seen, occlusion queries unavailable, some queries
unanswered, no motion grid (with the pass's reason). Zero rows is therefore never silent: the frame line says whether no eye draw
arrived (a menu, no VR), none was a candidate, or none reached the window.

## Flight plan

1. Set `fix.particle_billboard = stock` in `edvr.ini` (the live ini is Sean's: edit it with the Edit tool, never a regex). The probe offers
   each draw before any verdict substitutes its shaders, so it would see the game's hashes either way; stock removes the one other
   difference, the billboards' orientation, from the picture being judged. Bind `hotkey.blend_probe` to a free bare key, for example F3 (avoid keys the game uses).
2. For each element in the order 1 to 5, look straight at it so it sits at the centre of the view, press the key once while still, and
   once more (after the report, a second or so later) while moving the head. Note the order; the log numbers the captures by time.
3. Element 1: stand in the station hangar with the pad under the hologram. Element 3: adverts inside a station, and the two rows outside a
   large station. Element 4: hold fire at a dark target and press during the burst. Element 5: at the mail slot.
4. Element 2: press during the pad's raise and again during its lower, with the railing at the centre of the view.
5. The `temporal_aa_debug = motion_source` view of the railing cannot be taken on this build (see Status). The grid's `p` and `e` columns
   are its replacement.
6. After the flight: `python tools\edvr_log.py --target steam --expect-build HEAD --version`, then read the `blend probe` lines.

## Reading the output

- The row to look for in G1 is a blend-enabled draw with `depth=[... write 0]` that reaches the window with `cov` in the hundreds or
  more. Its `vs`/`ps` name the family; `ui=` and `holo=` and `mask=` say which of EDVR's routes already touch it.
- A row with `ui=...left:...` and `mask=-` and `holo=-` is in the game's frame with nothing telling DLSS it is a UI-like effect.
- The grid's `z` over those cells: equal to the far wall's depth with the wall's motion means the effect inherits the background (G1's
  mechanism). The moving head presses should show the wall's motion `mv` on those cells, and it should differ from the still presses.
- G2: `p` 1 or 2 on the railing cells while the pad moves means the camera term alone.

## Journal

- **2026-10-14, build.** `blend_probe_core.h` holds the window, the candidate rule, the text and the grid indexing; `blend_probe.cpp`
  the device side. Hooks: vscreen.cpp `forwardWithVerdict` offers each eye draw after its routing is decided (and at the skip return),
  `drawGateSubscribed` includes the probe so the verdict ladder runs and the AA-off fast path is not taken while it collects,
  `vScreenFrameBoundary` ticks it; temporal_pass.cpp asks for the capture-only decision shader and offers the staged crop after eye 0's
  motion dispatch (and releases the 65 MB decision texture once no eye run or capture wants it); ui_depth.cpp `uiDepthDrawProbe` and
  ui_layer.cpp `uiLayerDecisionCount` expose per-draw state read-only. The full build is green with the rig in its gate; nothing was
  installed or flown. The probe is VR only; a flat press is refused with a reason.
