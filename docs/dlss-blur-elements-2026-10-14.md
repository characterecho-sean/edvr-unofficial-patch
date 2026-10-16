# Five elements that look indistinct under DLSS (2026-10-14)

## Status

- **Shield-strike smear (2026-10-16): DESIGN ONLY, nothing built.** The shield's pool meshes (5C1D, 820E) are unkeyed, and they write no
  depth, so the engine path's ownership test rejects them. The design ranks a coverage replay that writes private depth plus the slot first.
  See "Design, 2026-10-16: engine motion for the shield strike". Evidence E1-E3 done offline (run 070301): option (b) CLEARED
  with one amendment, the shield records must also be joined (they share the hull's pose but carry no previous pose). H-clobber RULED
  OUT; the hull's 13% stale_slot is open hypothesis H-bias (a depth bias missing from the exported z). E2 (do puffs ride the ship) inconclusive.
  Build stopped: the shield records come from an unhooked producer, candidate FUN_14434D470/434D120 (see "Producer hunt"); count-and-tag hook next.
- **Weapons fire (2026-10-16): DESIGN ONLY:** the HDR-layer route (the space dust's) for 1F17, 939D and 5C1D; 820E stays. Evidence W1-W5
  offline first. See "Design, 2026-10-16: weapons fire after the upscale".
- **Sixth case, SRV driving (2026-10-14): ROOT CAUSE, fix BUILT, NOT FLOWN** (`claude/srv-split`). Terrain 5 to 8 m from an SRV took the head
  path because the ship's 10 m cockpit radius applied to the SRV; now 3.5 m in a seated SRV. Not one of the five elements; see
  docs/per-object-motion.md "The split in an SRV, 2026-10-14". The same question for a docking slot inside 10 m of a ship cockpit is open and unmeasured.
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

## Design, 2026-10-16: engine motion for the shield strike

Read-only design; no code. Evidence: Frontier `edvr_gfx_20261016_070029.log` (139a6ff2), eye runs 070301 (beam on a shield) and
070559. `tools\eye_decisions.py` on the shield regions gives world 100%, no valid depth, engine "unavailable", 2.5-6.6 px. The hull beside
them is 87% engine "joined" and 13% `stale_slot`.

**How the engine path works today.** Keyed pool families (`engine_velocity_families.h` `kFamilies`) are substituted at the draw:
- Their PS is patched (`dxbc_engine_velocity.h`) to export MRT6 = (2·slot+1, SV_Position.z), under a derived blend with target 6
  unblended RG (`engine_velocity_state.h`).
- The UV-only family 5B4D/4375 also gets a patched VS that exports `EDVRPOOLSLOT`. That patch is BC3E/BA58 in the census.
- The temporal pass's `enginePixelZ` (`temporal_shader_source.h:551-608`) owns a pixel only when the scene depth `zr` equals ES.y bit for
  bit. It then reprojects the point at `zr` through the record's current and previous pose. A marked pixel that doesn't own the depth
  returns code 4 (`stale_slot`); an unmarked pixel takes the camera path.

**Why the shield gets nothing.** Two reasons, each enough on its own:
1. 5C1D/C49F and 820E/6EAA are not in `kFamilies`, so they are never patched.
2. Even if keyed, they write no depth (`17wZ`), so ES.y ≠ `zr` and the consumer rejects them.

Corollary, hypothesis H-clobber: a keyed translucent draw (the 5B4D family's alpha draws, BC3E/BA58, in 070301/070559 only) overwrites
the hull's valid ES with its own non-owning depth. That would explain the hull's 13% `stale_slot`.

**Options, ranked.**
1. **(b) Coverage replay with depth and slot.** This is the `ui_depth.cpp` pattern used for the drive smoke (`kSmokeVs`, `kSmokeDepthHlsl`)
   and the reticle (`kHoloWorldMarkers`).
   - Hook: `ui_depth`'s coverage pass, after each 5C1D/820E draw.
   - The replay draw uses EDVR's PS: a transcription of the effect PS's alpha (C49F999F, 6EAA86EF), which discards below a floor and
     writes the pass's private depth plus ES (slot from v0.x, depth = the same z). Ownership then holds with the consumer unchanged.
   - Over the hull, the shield's record motion replaces the hull's. Both ride the ship, which E1 must confirm.
   - Over the sky, stars inside the covered core take the ship's motion. The floor bounds that.
   - Cost: one replay per shield draw (a few 2880-index draws), about 0.01-0.03 ms GPU; CPU one draw each. Shader work: two alpha
     transcriptions plus a slot-exporting VS, which the patcher already makes for 5B4D.
2. **(a) A translucent-owner channel.** Patch the effect PSs as pool producers that write the slot and their own z to a new target
   (MRT4/5 if free; MRT6 would clobber the hull, see H-clobber). The consumer reconstructs at that z, not `zr`, when it lies nearer than
   `zr`.
   - More general (any keyed translucent family), but it changes `enginePixelZ` and the bind validation, and needs the corpus identity
     harness for each PS.
   - It has no coverage floor unless the PS computes one, so faint additive fringes would claim pixels. Over sky that ghosts the stars.
   - Cost: one extra Load per pixel for each eye, about 0.03 ms.
3. **(c) Composite the effect after DLSS (UI layer).** Crisp, but it loses AA and jitter on a 2880-triangle mesh, and costs a re-render at
   output resolution. Better kept for beams and tracers (1F17, 939D), which have no record.

**DLSS preset K.** It ignores the reactive mask (Finding 1), so correct motion is the only in-DLSS lever. With the target's motion, K
keeps the effect's history instead of rejecting or ghosting it. The see-through background remains a translucency limit no motion fixes.

**Impact puffs and sparks** (AFBC/9F4B/BBAD, 9AEC). They come from a CPU particle stream with no t33 record, so this mechanism cannot
give them object motion. They need coverage depth (the camera path at their own depth, the smoke pattern), and object motion only if E2
shows they ride the ship. Their VS is already EDVR's (the facing transcriptions), so a replay can reuse it.

**VR and flat.** This design targets the VR temporal pass and `ui_depth`, which is VR-only. Flat consumes engine slots through its own
ownership path (`flat_foreground_ownership.h`, OwnerDepth == ConsumerDepth), which has the same equality, so the same replay applies there.
That is a separate change, and needs a flat combat capture.

**Evidence before building.**
- **E1.** Do the shield draws' t33 slots name records whose pose delta matches the hull's, and are those records "joined"? Run
  `tools\eye_run_ledger.py` on run 070301's pool, draw and record files. It needs no flight.
- **E2.** Do the impact puffs ride the ship? Track their screen offset from the hull across P00-P02 of 070301.
- **E3.** H-clobber: map 070301's `stale_slot` hull pixels (D00) against the translucent 5B4D coverage. If confirmed, (b) must also keep a
  translucent keyed draw from writing ES over an owned pixel.
- **Rig.** `tools\engine_velocity_test` on WARP: an opaque hull record, plus the shield replay over the hull and over the sky. Assert
  per-pixel ownership, a camera path below the floor, and an unchanged hull ES where nothing is replayed.
**Evidence, 2026-10-16 (run 070301, offline).** E1: the shield slots 157/162 (5C1D) and 119/124 (820E) share the ship's root pose with
hull records 200/220/241: identical position and per-frame change, 0.903°. They are NOT joined (marker 447BFA77, previous pose =
current), and the hull records are; so option (b) must also join the shield records (key them as producers, or have the replay register
its records for the join). Pointing the replay at the co-located joined hull slot instead is the ruled-out "pool slot as identity".
E2: inconclusive (the effects grow, so centroids are biased); needs a stride-92 vertex-buffer snapshot in the eye-run ledger.
E3: 0 of D00's 13,258 `stale_slot` pixels carry a 5B4D slot. They are slot 238 (cockpit, 10,775) and slot 220 (target hull, 2,483),
where the recorded depth is nearer than the scene's by 5e-7 to 1e-5, never farther. Open hypothesis H-bias: a depth bias missing
from the exported z, or a later non-GEQUAL depth pass; test by logging the rasterizer DepthBias/SlopeScaledDepthBias and depth function
of the keyed draws for slots 220 and 238.
- ruled out: translucent keyed 5B4D draws overwriting the hull's slot (H-clobber), because no non-owning pixel carries a 5B4D slot.
**Build attempt stopped, 2026-10-16 (before any code).** Two design premises were false:
- ruled out: keying 5C1D/820E as PS producers as the join, because the join is a CPU emission from the hooked rigid producers
  (engine_velocity_emit.h observe/observePrimary into the pool-clone sink) and a PS patch writes no previous pose.
- ruled out: a replay-registered join, because the only pose source the replay can reach is the co-located hull record (pose-match identity).
- finding: the shield records pass every structural test but never reach the hooked primary producer 42B4130 (relay 1215, joined 1212,
  declined 0), so an UNHOOKED producer builds them. The real join needs that producer identified in Elite's exe and hooked into the
  existing emission sink.
- finding: 6EAA writes constant alpha 1.0 and is a refraction pass (samples the eye HDR copy at screen UV plus a normal-map ripple),
  so there is no alpha to transcribe; coverage for 820E would have to be displacement-based, and object motion for refracted content
  is unproven. 5C1D/C49F is ONE+ONE with constant alpha 1.0, so its coverage floor is on HDR intensity (alpha*colour*cb1[90].y).
**Producer hunt, 2026-10-16 (offline: decompiles, capstone on PE 1788384820, run 070301).** In frame 15468, 162 slots are drawn: all 85
joined records carry the model+0x478 block at record +0x20..+0x9F, and 70 of the 77 unjoined have it zeroed. The rigid producers (42B4130,
42B4420 inline, 369C9C0) copy that block; FUN_14434D470 (matrix to packed quaternion, scale at +0x50) does not, and appends through
FUN_14434D120 (bucket, model key, record, node key), which nothing in EDVR hooks or counts. The four shield slots and the target-anchored
records 118 (E508) and 156 (1989) share the stack-leftover +0x120 word 0x447BF39C: one producer call. Candidate producer:
FUN_14434D470/434D120 (prologue 48895C2408 48896C2410 4889742418 57 4883EC20). Not yet excluded: the other initializer callers
1431FA800, 143C8F8C0, 143DA1DE0, 143E7AF20, 1443ED820. Next: a count-and-tag hook on 434D120 (calls, buckets, threads, overlap with the
rigid producers' buckets, appended addresses mapped to pool slots in an eye run) before any emission. "139 of 2048 joined" counts pool
capacity, not a miss rate.
- ruled out: FUN_14369C9C0 as the shield's producer, because it made 0 calls this session and it copies the model block.
## Design, 2026-10-16: weapons fire after the upscale (the HDR layer route)

Read-only design; no code. The route already exists: the space dust (vs 9BFC7FD2 / ps DBF17257) is an additive, GEQUAL, no-write world
effect in the lit HDR target, taken into the per-eye HDR layer since 2026-10-07 (docs/ui-layer-2026-09-23.md, FLOWN OK b4d9d6ab).
Weapons fire has the same shape.

**What the route does.** The game's own draw goes into an R16G16B16A16F layer per eye at the DLSS output size times the `fix.ui_quality`
target (4032² at 100 from a 2016² render); the viewport is remapped, NDC and the VS unchanged. Jitter is cancelled by a viewport offset
(`uiLayerJitterCancel`), valid because the VR jitter goes in at GetProjectionRaw. Depth is tested with the game's own function against a
nearest-sample, jitter-cancelled seed of the render-res depth (`ui_layer_seed.h`). The game's tonemap draw is re-issued with the layer as
its HDR source (game exposure and LUT), and the result composited onto the RCAS output per eye before the menu and the submit. DLSS never
sees the effect.

**Proposed families.** The exact pairs 1F17BF54/A75C1DB6 (beams, bolts), 939D01F2/C5DF9CC9 (multicannon rounds) and 5C1D8EF5/C49F999F
(shield-strike additive part), as one family `weapons fire` on the HDR branch of `uiLayerFamilyFor` and in `uiLayerFamilyTakesHdr`, exempt
from the density rule. SRC_ALPHA,ONE and ONE,ONE both convert exactly.

**What looks different, ranked.** (1) No bloom halo if bloom reads the HDR target after these draws (bloom BELIEVED baked in before the
tonemap, never hashed). (2) Additive after the tonemap: T(F)+T(L) != T(F+L); exact over black space, brighter or clipped over a planet or a
star (dust p90 6/255, p99 69). (3) Draw order: a later depth writer over beam pixels would be a defect; a later alpha layer no longer dims
the beams. (4) A one-render-pixel occlusion step at hull silhouettes. (5) Planetary fog skipped. (6) 820E's refracted copy no longer holds
the beams.

**Alternatives.** A coverage mask with pinned or own motion: no motion source, and K ignores the mask. Drawing twice and subtracting:
ghosts, additive light counts twice. A private-depth replica: camera motion at the effect's own depth, not its own motion. Kept for stage
2: feed only the bloom chain from a private HDR copy, if the halo is missed.

**Evidence before building (offline, runs 070301 and 070422, log 070029).** W1: no depth writer over the weapon pixels after the weapon
draws and before the tonemap. W2: the first reader of the lit HDR target after them (the bloom chain), and the halo energy. W3: 1F17's
CB1[41..48] and 939D's and 5C1D's CB0 rows 4-7 carry the eye jitter. W4: tone-order parity over the beam pixels. W5: the RTV is the lit HDR
eye target, the DSV is seedable, and the stencil state.

**Flight (after the build).** Frontier VR, DLSS K, `fix.ui_quality` 100. Fire beams and multicannon over space, a planet and near a star,
head still then moving; hit a shield; eye dump during a burst. Pass: the 30 s `weapons fire` line at 2 or more a frame while firing with 0
left in the scene; no trails; beams cut by hulls and never over the cockpit; the dump's tonemap input without the beams. Then Sean judges
the halo.

- ruled out: 820E/6EAA in the layer, because it samples the pre-DLSS eye HDR copy at the screen UV; it stays in the scene.
- ruled out: drawing the effects at render size and upscaling the layer, because it is the blur DLSS already gives, without DLSS.
