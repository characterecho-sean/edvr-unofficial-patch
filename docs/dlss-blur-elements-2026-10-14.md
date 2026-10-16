# Five elements that look indistinct under DLSS (2026-10-14)

## Status

- **Shield-strike smear (2026-10-16): DESIGN ONLY, nothing built.** The shield's pool meshes (5C1D, 820E) are unkeyed, and they write no
  depth, so the engine path's ownership test rejects them. The design ranks a coverage replay that writes private depth plus the slot first.
  See "Design, 2026-10-16: engine motion for the shield strike". Evidence E1-E3 done offline (run 070301): option (b) CLEARED
  with one amendment, the shield records must also be joined (they share the hull's pose but carry no previous pose). H-clobber RULED
  OUT; the hull's 13% stale_slot is open hypothesis H-bias (a depth bias missing from the exported z). E2 (do puffs ride the ship) inconclusive.
  Build stopped: the shield records come from an unhooked producer, candidate FUN_14434D470/434D120 (see "Producer hunt"). A count-and-tag probe
  (`claude/producer-434d120-probe`) confirmed FUN_14434D470 makes the shield (see the flight notes); the probe and the parked join are NOT on main.
- **Shield refraction after the upscale (2026-10-16): BUILT, NOT FLOWN** (`claude/shield-ripple`, VR only; entry "Built, 2026-10-16: the shield
  refraction"). 820E is skipped in the scene and its displacement applied after RCAS via a per-eye layer D. Sean approved shipping it under
  `fix.sharp_weapons`; `advanced.shield_ripple` and the coverage instrument are REMOVED. R1 (gain, filter) runs live and stands down to stock with a log line.
- **Flat port (2026-10-16): BUILT, NOT FLOWN.** Weapons and ripple use flat's adapter; pre-RC3 F3/F4 full gate passed (222.4 s); entry below.
  No flat combat capture exists; open: do flat's weapons draws carry camera rows (`flat ui layer weapons`)? See "Flat port".
- **Weapons fire (2026-10-16): MERGED** (`claude/billboards-sharp-weapons`; flown OK in combat as `claude/combat-flight-1`; entries "Built" and
  "Combat flights"). 1F17, 939D and 5C1D go into the eye's own additive HDR layer, composited after the upscale by tonemap difference; 820E stays.
  Sean: weapons and shields look much better, beams and tracers are sharp. **ONE key, `fix.sharp_weapons = on | plain | off`** (default on, independent of
  `fix.ui_quality`; menu: one "Sharp weapons" row, On / On (plain blend) / Off (stock), directly under UI quality, dimmed with temporal AA off). on = the
  matched composite, plain = the plain add, off = all stock, shield ripple included. `advanced.sharp_weapons_blend` is MERGED INTO IT and removed (with its
  menu row and its `MENU_ADVANCED_EXCEPTIONS` entry): an old line is read by nothing, and the old true/yes/1 and false/no/0 spellings of the key still work.
  The blend A/B showed no difference live (log edvr_gfx_20261016_095256). W3 SETTLED: all three pairs carry the eye's jitter (dump 100257), the cancel table says cancel for each. The 0.5 to 5 percent "layers'
  content never reached a tonemap" was an accounting artifact (one false count per burst end), nothing was dropped; the corrected count
  ("The never-tonemapped frames: found") is merged but NOT yet flown.
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
**Flight fdb42b63 (2026-10-16 09:47, Frontier VR, eye run 094919; claude/combat-flight-1).** Producer probe 434D120: hooked; 1 bucket,
render thread only, never a rigid bucket; caller 0x434D741 (FUN_14434D470); every record zero-block. It produces the cockpit and HUD
pool records (2CEC 30, 1989 22, E508 8, 5DAD 2 of 67 tagged slots). No shield drew in the dump, so the shield link is still indirect
(070301's shared +0x120 fingerprint); next, an eye dump during a shield hit. Sharp weapons: no draws left in the scene, no fallbacks,
matched only (no A/B yet); Sean: "weapons/shields look much better", beams and tracers sharp. 0.5-2.6% of weapon eye-frames' layer
content never reached a tonemap (possible occasional bolt flicker; open). W3: 939D's CB0 rows 4-7 equal the scene's jittered
cb1[270..273] bit for bit apart from translation, so its cancel is right; 1F17 and 5C1D were not captured. Explosion: drawn by the five
transcriptions only; B477 and E812 were not created this session. New untranscribed billboard VS dumped: F5CF and A958 (fade and
no-fade, 9F4B layout), EF5D and 5218 (BBAD layout), 83E9 (9AEC layout); they need PS names from a census before transcription.
**Flight fdb42b63, session 095256 (2026-10-16 10:02-10:05).** W3 settled: 1F17's `cb1[41..43]`, and 5C1D's and 939D's CB0 rows 4-7,
equal the jittered scene `cb1[270..272]` bit for bit apart from translation (dump 100257), so jitter cancel is right for all three pairs.
Shield producer CONFIRMED: in 100257 the 5C1D slots 69/70 and 820E slots 109/110 are 434D120-appended records (bucket 2312D2D2A50, one
model key 2317A8D34F0, stable for 19 frames, render thread); FUN_14434D470 makes the shield. Smoke dumps 100331/100415/100454: every
billboard draw went through a facing transcription (100% swapped); no untranscribed billboard and no B477/E812 drew; 6A19 is a pool mesh,
not a billboard. Sharp weapons: 1.4-4.7% of weapon eye-frames' layers never reach a tonemap (the HUD layer lost none); the flown build
cannot locate it. Sean A/B'd plain against matched at 09:54-09:57 and saw no difference; the key stays for long-session A/B.

## Built, 2026-10-16: weapons fire in its own HDR layer (`claude/sharp-weapons`, MERGED, flown OK in combat)

The design above, built. Key `fix.sharp_weapons = on | off` (default on, F8 Performance page "Sharp weapons"), VR only, needs temporal AA on. A
temporary A/B, `advanced.sharp_weapons_blend = matched | plain` (default matched, hidden), is listed in the Status block and goes when the arc closes.

**What it does.** The three exact pairs 1F17BF54/A75C1DB6 (beams, bolts), 939D01F2/C5DF9CC9 (multicannon) and 5C1D8EF5/C49F999F (the shield strike's
additive part) are family `weapons fire` (`weapons_fire.h`; `UiLayerFamily::kWeaponsFire`, last in its enum). It is on the HDR branch of
`uiLayerFamilyFor`, in `uiLayerFamilyTakesHdr`, not in `uiLayerFamilyIsSceneLines` (exempt from the density rule). vscreen.cpp reads the pixel
shader for these vertex shaders; 820E/6EAA stays in the scene. The draw goes through the space dust's route (seeded depth, GEQUAL/GREATER no write,
the viewport remap with the jitter cancel), but into the eye's OWN additive HDR layer W (R16G16B16A16_FLOAT, the HUD layer's size), not the HUD's.

**The toggle is independent of `fix.ui_quality`.** `refreshLive()` computes two halves: `quality = uiLayerLiveFor(g_target, ...)` (unchanged, now
`detail::g_uiQualityLive`) and `g_weaponsLive = uiLayerWeaponsLiveFor(fix.sharp_weapons, temporal, ..., not flat)`. `detail::g_uiLayerLive` (the
machinery: the door, the HDR tonemap re-issue, the composite) is their union. A family is decided only if its own key is on
(`uiLayerFamilyEnabled`; new decision `family-off`, tested right after the verdict). The parts that are the interface's stay on the first half: the 2D
screen and the post-tonemap rules (`uiLayerFamilyOf` returns none for the 8-bit target), the VR world route and the on-foot gate
(`uiLayerLiveForWorldRoute`), the composite census, the after-UI take (`uiLayerNoteOther` returns after its seed check). With `fix.ui_quality` off the
layer size is the DLSS output times 1.0, and at the frame boundary the interface's HDR layer, transmittance and depth are released. All four
combinations are in `tools\sharp_weapons_test` (the family gate, the decisions on every family and on the recorded rows, the two liveness halves).

**A2, the brightness fix (matched).** Per eye-frame with weapons content, the layer re-issues the game's own tonemap draw (its exposure, LUT and b2,
nothing rebound but the target, the blend and the HDR source) over the 8-bit layer E: clear E to (0,0,0,1); one source pass (`sharp_weapons.h`,
fullscreen, two targets, the viewport the layer's) writes S = F_up (the tonemap's HDR input resampled to the layer's size by a bilinear made of Loads, so
the game's sampler is not involved) and adds the same F_up onto W, so W = L + F_up; then tonemap(S) replaces E, and tonemap(W) is drawn with blend
SUBTRACT (source minus E), which leaves T(F_up + L) - T(F_up) in E (8-bit, clamped at 0); the HUD's tonemap, when the HUD layer has content, is then
ADDED (ONE, ONE) so it does not erase the weapons. The door's existing composite adds E onto the post-RCAS picture (alpha one, no coverage). `plain` is
`E = T(W)` with W holding L alone. `sharpTonePlan` is the pure plan (`vscreen.cpp` issues the game's draw once per step through
`uiLayerCrispToneNext`); a matched frame that has no scratch, shaders or deferred context falls back to plain, counted. S is one scratch for both eyes.
- **Proof (WARP, `sharp_weapons_test`, a synthetic tonemap with a scalar exposure, a 3D LUT and a compressing curve, so T(F+L) != T(F)+T(L)):** over a lit
  background the matched composite is within 1/255 of T(F + L), the plain add 71/255 off; over black both are within 1. Mutants of the sequence (the
  source pass replacing instead of adding, the lit step replacing, the base step missing, the lit step reading the scratch) are off by 44, 71, 71, 71.
  S is exact to the CPU resample, W within 0.002 of L + F_up. The source pass leaves the immediate context's targets, viewport and shaders as it found
  them, and the debug layer reports nothing. Needs the game's tonemap to read its HDR source by the vertex stream's UV (its layout is POSITION +
  TEXCOORD; the HUD's re-issue has flown on the same assumption).

**A1, the watch.** `eye_draw_snapshot.h` watches 1F17BF54, 939D01F2 and 5C1D8EF5, so the next eye dump holds their VS b0/b1/b2. W3 stays OPEN until
1F17's CB1[41..48] and 939D's and 5C1D's CB0 rows 4-7 are compared with the jittered scene camera's matrix of the same frame. The cancel is read per pair
from `kWeaponPairs[].cancelJitter` (default true, as the dust): `weaponsFireJitterCancel(vs)` sets `g_draw.jx/jy` to zero for a pair marked false. If a
pair is unjittered, set its entry false and nothing else.

**Log lines** (all `sharp weapons:`; each has a not-exercised form). Once per change: on or off with the state of `fix.ui_quality`, the blend. First
sight of each pair: `first <pair> draw seen (vs .. ps ..): <decision>; jitter cancel yes|NO`. Every 30 s: `<K> draws a frame in the HDR layer, <L> a
frame left in the scene (reasons), blend matched|plain; eye-frames: N matched, N plain, N plain after a matched frame fell back; N layers' content never
reached a tonemap; draws by pair: ...`, or `not exercised in this window (no weapons-fire draw reached the layer's decision)`, or `not live this window
(fix.temporal_aa is off ...)`. First composite of each kind, the first fall-back, the scratch and each weapons layer when created.

**Not changed / accepted.** The weapons' light is added to E independently of the HUD's coverage: a translucent HUD over a beam no longer dims it (W1
accepted that later over-blends no longer dim the beams). The matched difference is made from two 8-bit results, so it carries +-1/255. T(0) is
taken as 0, as the HUD route does. A failure of the weapons layer stands down the weapons alone (`sharp weapons: stands down`); the missing tonemap
consumer stands down the shared re-issue, HUD and weapons both.

**Flight (Frontier VR, DLSS K).** Read the log with `python tools\edvr_log.py --target frontier --expect-build HEAD`, then grep `sharp weapons`. Fire
beams and multicannon over space, a planet and near a star, head still then moving; hit a shield; an eye dump during a burst for W3. Pass: the 30 s
line at 2 or more a frame while firing with 0 left in the scene, no trails, beams cut by hulls and never over the cockpit, the beam over a planet or
star as bright as stock (A/B: `advanced.sharp_weapons_blend = plain` is brighter there), the dump's tonemap input without the beams. Repeat with
`fix.ui_quality = off`, and with `fix.sharp_weapons = off` for the control. Then Sean judges the halo (no bloom in this tier, W2).

## Combat flights, 2026-10-16, and what followed (`claude/combat-flight-2`, MERGED)

**Flown** (build fdb42b63, logs edvr_gfx_20261016_094708 and 095256). Sean: weapons and shields look much better, beams and tracers are sharp. He switched
`advanced.sharp_weapons_blend` between plain and matched live (the 30 s line shows both kinds of eye-frame in 095256) and sees no difference; he keeps the
A/B. W3 for 939D: its CB0 rows 4-7 equal the scene's jittered cb1[270..273] bit for bit apart from the translation, so the default cancel is right for it.

**The menu row.** The A/B is the first row of the Performance page, the page the menu opens on: `# ui: Weapons blend | choices matched=Matched, plain=Plain |
live | menu performance | top` above `#sharp_weapons_blend` in [advanced]. Two changes to tools\gen_settings_schema.py: a `top` token on a ui: line sorts a
row first on its page (stable, every other row keeps the ini's order), and `MENU_ADVANCED_EXCEPTIONS` (one key, TEMPORARY) lets that [advanced] key carry a
`menu` token as a Fix-tier row instead of being refused, skipped in the developer tier so it is one row. The menu code is unchanged: it lists the table in
order. Held by the generator's self-test (three cases) and by `sharp_weapons_test` on the real generated table.

**The never-tonemapped frames: cause OPEN, instrument built.** From the two logs: a weapons layer counted lost when the eye's next weapons draw clears it
with its content not published (`wToneSeq != wSeq`). 18, 8 and 44 in the three 30 s windows of 094708 (390, 3204 and 1709 eye-frames) and 2, 2, 8, 4, 4, 0
in 095256. The rate is highest in the beam-only windows (4.6 and 5.5 percent of 390 and 36 eye-frames) and lowest in the multicannon-heavy one (0.25
percent). The HUD layer, in the same frames, lost none (0 in every window; a re-issue for every eye-frame that had HUD content), and no re-issue decline was counted, so the re-issue
ran for the eye-frame; "left in the scene" holds only one `no-eye` draw. So a tonemap ran but did not take the weapons content. What the code allows, each
with its signature: (a) the tonemap read a different frame number than the weapons draws carried (`wFresh`, `wpnC` compare `wSeq` with the tonemap's
frame); (b) no tonemap-shaped draw ran for that eye; (c) the first weapons draw of the frame went into another HDR target than the one the tonemap reads
(`wTarget` is the first draw's, and the admission matches the weapons content by it when the HUD layer has none that frame); (d) a refusal. Ruled out: the
tonemap arriving before the weapons draw (that is `tone-late`, a decision counted in the left-in-the-scene list, which has none), and a second tonemap or a
busy 8-bit layer (counted declines, none). The instrument: every lost layer prints (the first 24 a session) `sharp weapons: lost layer N`: the eye, the
content's frame, its draws, the target of the first and how many went into another, the HUD layer's frame and whether it was tonemapped, the next weapons
draw's frame, and the last six tonemap-shaped draws the admission saw (frame, resource read, eye matched, outcome). Next flight: read those lines; (a)
shows as a tonemap entry one frame off the content's, (b) as no entry for the eye, (c) as a nonzero "into another target" or a read resource unlike the
first-draw target. The fix follows the signature (a: the tonemap uses the eye's own frame; b: composite the leftover content at the door; c: match by
the layer's own target set), with a rig case that fails first.

### The never-tonemapped frames: found (`claude/combat-flight-3`, MERGED, accounting fix NOT FLOWN)

**Cause.** Flight edvr_gfx_20261016_103639 (build a5709ce1), ten `lost layer` lines. Every one has the same shape: a weapons layer holding ONE draw (frame 5675,
1 draw, first into the eye's own target, 0 into another), the next weapons draw ~200 frames later (frame 5879), and between them the tonemap-shaped draws of
both eyes admitted every frame (the HUD's re-issue runs every frame). Lost layers 3 and 4 are the same (content 5887, next draw 6678). The count is made at the
eye's NEXT weapons draw (`beginInner`, when the layer is cleared for the new frame), and compared the content's frame with `wToneSeq`, which every re-issue
overwrites: by the time a burst's last draw is followed by quiet frames, `wToneSeq` is the latest frame, never the content's, so the next burst's first draw
counted the previous burst's last draw as lost. One false count per burst end per eye; that is why beam-only windows (many short bursts) showed 4 to 5
percent and the multicannon-heavy one 0.25. The hypotheses, against the code: (1) a final draw landing after its frame's tonemap is `tone-late`, a decision
counted in the left-in-the-scene list, which had none; (2) there is no state where publication waits for a later frame: the admission matches the content by
the eye's own frame and target in the frame it is drawn (`wFresh`), whatever the draw count, and the bracket carried the layer in that same re-issue; (3) the
frame tag is stamped at the layer's first draw from the eye's own sequence, the one the tonemap reads. A lone final draw was composited in its own frame's
tonemap all along. No tighter instrument was needed: the lines' own numbers (one draw, a ~200-frame gap, admitted tonemaps in between) show it.

**Fix.** The accounting, not the compositing. A new tag `wPubSeq` is stamped only by a re-issue whose bracket carried the weapons layer's content
(`uiLayerCrispToneEnd`, `hadWeapons`), and a layer is dropped iff `weaponsLayerDropped(contentFrame, draws, wPubSeq)` (weapons_fire.h). The miss streak
(30 frames of unpublished content stands the weapons path down) resets only on such a bracket, so a real persistent failure can still reach it, which the
old unconditional reset made impossible. The 30 s line now prints "of N eye-frames with weapons draws" beside the composites, so the identity
`matched + plain + fell back == eye-frames with draws - layers lost` can be read off one line. Not changed: the HUD layer's own count
(`hdrLost`, `hdrToneSeq`) has the same shape of comparison but its layer is drawn every frame, so it never showed; if the HUD is ever drawn in bursts it
needs the same tag.

**Rig** (`sharp_weapons_test`, `dropTests`): a burst of five three-draw frames and a lone final draw, 200 quiet frames with the tonemap running, then the next
burst: the lone draw is carried by its own frame's re-issue, the production rule counts 0 lost and the frozen rule 1 (fail-before). Fifty lone draws 30 frames
apart: production 0, frozen 49. A real drop (content, no re-issue in its frame) counts 1, and so does a re-issue that ran in the content's frame without
carrying it, which the frozen rule could not see (0). A continuous burst counts 0 by both. Pins with controls: the production test is
`weaponsLayerDropped(e.wSeq, e.wDraws, e.wPubSeq)`, the stamp sits inside `if (hadWeapons)`.
## Design, 2026-10-16: the shield refraction after the upscale (820E5C13/6EAA86EF), BUILT (entry below), NOT FLOWN

**What 6EAA computes** (disassembled from edvr_logs\shaders). It sums n = (0,0,1) plus, for each of `cb2[7].w` hits, the t1 normal (x2-1,
faded by `saturate(0.5*x1.x)`, weighted by `saturate(dot(normalize(hitpos), v1))`), and samples `t0` at `uv_jit + normalize(n).xy*cb2[0].x`,
where `uv_jit = v0.xy/v0.z*(0.5,-0.5)+0.5` and v0 is the jittered clip position. It writes `x cb1[90].z x cb1[90].y` with alpha 1 (an
overwrite). In 100257 the frame CB row 90 is (0, 1/256, 256, 0), so the gain is exactly 1 (INFERRED for 820E's PS b1, not captured). No
tint, fresnel, additive or chromatic term. d is a UV fraction, so it scales to any resolution.

**Commute.** The tonemap 99C2 is one t1 tap, then exposure, LUT and saturation, plus a +-0.5/255 position dither: T(F)(x+d) = T(F(x+d))
within about 1/255. The break is order: the canopy 8C091FFD, F512712C, 025B4B9F and the puffs 9AEC/9F4B/AFBC/BBAD are drawn after 820E
unrippled in stock, and a post-DLSS ripple would ripple them.

**Design.** Skip 820E in the scene. Re-issue it unchanged, in-frame, through the HDR layer route (remap, jitter cancel, seeded GEQUAL)
into a cleared per-eye R32G32 layer D with t0 bound to a 2016² UV ramp, so D holds the game's own `uv_jit + d`. At the door, after RCAS and
before the E composite: `out = copy(RCAS).Sample(D - jitter_uv)` over D's coverage. Fallback: a DXBC patch writing d to RG16F. Temporary
A/B: ripple after the upscale, none, stock.

**Alternatives.** The UI resolve bound over 820E's coverage (fallback; softens the whole bubble); the parked join plus displacement coverage
(wrong for sky; the ripple animation keeps smearing); skipping with no ripple (diagnostic leg only).

**Evidence before shipping.** R1: watch 820E in eye_draw_snapshot (PS b1, PS b2, s0) for the gain, cb2[0].x and the filter. R2: the draws
after 820E that overlap the bubble (070301, 100257). R3: 820E draws per eye-frame. Rig: `tools\shield_ripple_test` (the game's ps_6EAA on a
ramp against the analytic uv+d; stock against post-path within 2/255; mutants for the jitter, the y flip, the d scale, the depth seed, the
copy, the order and a hole).


## Built, 2026-10-16: the shield refraction after the upscale (`claude/shield-ripple`, NOT FLOWN)

**Shipped (Sean approved, same day).** The first build carried a TEMPORARY A/B key `advanced.shield_ripple = sharp | none | stock` and a coverage instrument;
both are gone. The ripple is `sharp` under `fix.sharp_weapons` (on and plain take it, off leaves 820E in the scene), with the R1 stand-down, D's idle release
and the rig kept; `advanced.sharp_weapons_blend` is merged into `fix.sharp_weapons = on | plain | off` (see Status). The text below describes the first
build; read "mode" as the key's on/off, and skip the instrument paragraph and the two instrument rig cases (the rig is now 386 checks). The six later pairs
(canopy, reticle and overlays) are rippled with the rest of the picture for now.

**What runs** (VR, behind temporal AA; key `advanced.shield_ripple`, default `sharp`). The exact pair 820E5C131B99361D / 6EAA86EFE135B2D4 is a
`UiLayerFamily::kShieldRipple` row on its own key (not fix.ui_quality's, not fix.sharp_weapons's). A taken draw is re-issued unchanged through the
HDR layer route (viewport remap, jitter cancel, seeded GEQUAL depth) into the eye's cleared R32G32_FLOAT layer D (cleared to -10000, so "no
bubble" is far outside [0, 1]), with blend disabled and t0 swapped for a shared render-size UV ramp (texel i holds (i + 0.5) / W). The game's own
shader then writes `uv_jit + d` into D. D is allocated on the first 820E draw and released after 300 idle frames. At the door, after RCAS and
BEFORE the weapons/HUD composite: on frames with D coverage the RCAS output is copied, and `out = copy.Sample(D.uv - jitter_uv)` is written over
D's coverage (linear, clamp). `none` skips 820E with no ripple; `stock` leaves the game's in-scene draw. A taken draw is never taken-then-dropped:
`rippleTakeReady` and the R1 gate run before the decision.

**R1 at runtime** (first 820E draw, then once more per re-arm): the PS b1 row 90 is read through an async staging copy (no stall; the draw stays
in the scene until it has been read) and the gain `cb1[90].z * cb1[90].y` must be 1 within 1e-6; s0's filter must be linear (0x14 or 0x15). A
failure logs `shield ripple: stands down for the rest of the session -- <reason>` and leaves stock; turning the key off and on re-arms. Log
lines: `shield ripple: first 820E draw -- PS b1 ..., s0 filter ...`, `PS b1 row 90 reads (...)`, the D lifetime lines, and a 30 s line with 820E
draws a frame, ripple frames, stand-downs and "not exercised" forms of each. eye_draw_snapshot now watches 820E (VS b0/b1/b2) for the next dump.

**The coverage instrument** (R2/R3 amendment: no do-not-ripple mask; the reticle and overlays stay rippled for now). On eye-frames with D coverage,
the six later in-scene pairs (8C091FFD canopy, 71DD8B8B reticle, 381D8028, 2CECEC30, 24214E7C, CC2BA2E2) are replayed once through the blend-probe
method (writes masked) with an occlusion query against a stencil that marks D's coverage; results are polled on a later frame (`DONOTFLUSH`, no
stall) and the covered samples per pair go in the 30 s line. The answer tells which later draws the ripple would have wrongly displaced.

**Rig** `tools\shield_ripple_test` (WARP, debug layer, in the build gate; 422 checks), with the game's real ps_6EAA bytecode as a fixture:
- The shader's arithmetic, t0 point-sampled on a 2016 ramp: max |D - nearest texel centre of (uv_jit + d)| = 1.04e-08 (a texel is 4.96e-04).
- **Not met as specified:** the ramp read back through the LINEAR sampler is within 2.4e-5 of uv + d (0.05 of a render texel), not 1e-6. WARP's
  fractional filter weights are coarse, and the machine's own adapter (`--hardware-self-test`, not a gate) gave the same 2.4e-5, so the 1e-6
  hardware figure in the design was wrong on this rig. The rig bounds it at 0.1 texel on WARP. Size: under 0.05 render px, far below the
  2/255 the route test needs; cause beyond "the texture unit's weights" not isolated.
- Route (stock T(6EAA over F) against the door path's displaced read of T(F)): max 2/255 over the bubble and the rest (8 of 4140 bubble samples
  off by 2, none by more), 0 on the overlay; second configuration (render 80, other jitter) 2.
- Mutants, each far above the catch threshold of 3: jitter not subtracted 6, ramp y flipped 50, d scaled by the layer/render ratio 4, no depth
  seed 5, in-place read without the copy 8, ripple after the weapons/HUD composite 112 (overlay 143), 820E skipped with D not admitted 5.
- Coverage instrument: a later draw overlapping the bubble's corner counts 220 samples (expected 220), one wholly inside 256 (256), one beside it 0.
- Source pins with controls (door before the early-out, t0 restored, D cleared outside [0, 1], R1 checks, idle release, key, vscreen hook,
  snapshot watch, the ini row with no `# ui:` line); the debug layer reports nothing.

**Not verified until a flight:** R1's gain is INFERRED as 1 from 100257 and is checked live; the real refraction offsets, the idle release in a
long session and the instrument's counts have never run in the game. Read the log with `python tools\edvr_log.py --target steam --expect-build HEAD`,
then grep `shield ripple`. Compare `sharp` against `stock` and `none` on the same strike.

## Flat port, 2026-10-16: `fix.sharp_weapons` in the flat edition (`claude/flat-sharp-weapons`, built on `claude/shield-ripple` and `claude/flat-debug-menu`, NOT FLOWN)

**Question.** Is the route sound for flat, and what differs from VR? Read from the code. No flat log holds a combat capture: of the 400 or so flat logs on this machine
one (edvr_gfx_20261015_155732) names a weapons pair at all, 5C1D/C49F (the shield strike's additive part) once, in a source-free census line with `same-camera=1` (its
camera rows equal the scene camera's, which is what the jitter read needs). Everything below is design and rig, and the flight decides.

**What is the same.** flat_ui_layer.cpp is a mono adapter over the VR layer's own take, tonemap re-issue, door and composite, driven as eye 0 (flat_ui_layer.h). The
weapons and the ripple use exactly that machinery: the draw is taken by the shared decision (`uiLayerDecide`) with the draw's measured jitter, drawn into the weapons layer
W or the displacement layer D at the door's size, the game's tonemap draw is re-issued with W at its HDR slot (matched: scene and scene plus light; plain: the light), and
at the game's output copy `uiLayerComposite` adds the light and, first, the ripple. The tonemap is the game's own pointwise draw in both profiles (E894 / 0D43 reading
the HDR target at t1 with a 2D bloom at t2, or the registry's pairs), so T(F_up + L) - T(F_up) is as valid in flat. The flat HDR route resolves H before the tonemap and the
composite still runs at the output copy, so the picture the weapons land on is T(F_up) in both routes. The depth seed, the jitter cancel through the viewport, the
GEQUAL test, R1 (the gain and s0's filter, read live) and D's idle release are the shared code.

**What differs, and what was done about it.**
1. *The take is gated by flat's own proofs.* A draw is taken only behind a tone pass proven the frame before to read its target (kToneUnproven), never after the tone
   (kAfterTone), only when the camera injector owns the jitter (kNotUpstream) and only with camera rows whose shift is the frame's phase or zero (kNoRows, kOtherShift). A
   refusal leaves the draw in the scene as stock, counted by reason. For the weapons that costs nothing (additive light: a pair left in the scene is as smeared as it
   was, and the layer sums with it); for 820E it means no ripple that frame, in the scene as stock. The flat recipes list 1F17 (CB1 41..48 and 45..48), 939D, 5C1D and 820E
   (CB0 row 4) as camera consumers, so rows are expected; the flight shows it in `no-camera-rows` and `other-shift`.
2. *The door arms a frame late* (flatUiLayerDoorArms): the first weapons draw after a refused frame stays in the scene.
3. *Flat's gate keys were fix.ui_quality's.* `flatUiLayerDecide` now asks the half of the layer that governs the family (`flatUiFamilyLive`): the weapons and the ripple
   with fix.ui_quality off take alone, and the HUD is not asked when only the weapons are live. The `!flat` terms of the liveness math are gone.
4. *The tone bracket was one issue.* The matched composite is up to three issues of the tone draw (the scene alone, the scene with the light, the HUD); flat's re-issue now
   loops `flatUiLayerToneNext` as VR's does.
5. *The picture at flat's door is not EDVR's to rewrite in place.* uiLayerComposite already writes its own output for flat; the ripple now does too: it copies the whole picture
   into EDVR's own texture, rewrites the covered pixels from the pre-rewrite copy (the same compute pass), and hands that texture on as the frame the weapons and the
   HUD composite over, or alone. The game's own picture is untouched (rig). A picture the resolve did not make (`treated` false) is not read at D minus the jitter and the
   ripple is skipped for that frame (counted: "a picture the game made itself").
6. *The panel* has a seventh row (kMenuMaxLines 17 to 18; the flat page, a blank line and the ten-line warning fill the card). Sharp weapons sits under UI quality and
   dims with the flat AA mode off (`weaponsRowDim`, the flat reading `requestedTemporalMode`).
7. *Logging.* The flat window prints `flat ui layer weapons:` (the two families asked, decided, taken, refused at issue, or "not exercised"); `sharp weapons:` and
   `shield ripple:` print in flat as in VR, with their not-exercised forms.

**Rig.** sharp_weapons_test (`flatTests`: the family rule, every combination of the three halves, the row and its place, the allow-list, the wiring pins with controls),
shield_ripple_test (the not-EDVR's-own route: the rewritten copy agrees within 2/255, the original unchanged, two mutants), flat_sharpen_test (the seven rows),
the existing flat gates.

**The flight.** Flat edition (Steam or Epic), fix.sharp_weapons on, flat AA on (DLSS, XeSS or TAA), with a hostile close enough for beams, multicannon and shield strikes:
about two minutes of firing and taking fire, ideally once with fix.ui_quality on and once off, and the same strike with the row on, plain and off from the panel (live).
Send the log. Read: `flat ui layer weapons` taken against asked, the refusal counts, `sharp weapons: the left eye's weapons light was composited` first-seen lines,
the `shield ripple` first-draw lines (R1 gain and filter) and the 30 s line; and look at a shield strike and a beam over a planet with the row on and off.

**Pre-RC3 review fixes, 2026-10-16 (full gate passed).** F3: the flat tone
consumer required a fresh HUD even when it had taken weapons into W. It now
accepts fresh HUD or weapon content, matching that content's HDR target or
its proven copy. A copy also names its source so an unrelated target cannot
pass admission. F4: an actual D3D11 device replacement now releases the
sharp-source shaders, all four weapon blend states, and the shield ripple's
device objects, then clears creation attempts and first-draw validation.
The device regression warms both paths on A, resets, warms B, and checks
every retained child's owner. Same-device resize keeps its existing path.
The production flat adapter/layer regression passes 73 WARP checks (on/plain,
HUD quality off or HUD-free quality on, copy routing, refusal controls and
rendered composite); the two-device regression passes 53. Both are in the
full build gate. The absolute-path `build.bat --wait-lock` passed in 222.4 s:
165 pooled jobs, six quiet jobs, production DLLs, installer and final receipt.
These changes do not depend on a headset or VR runtime. Flat combat remains
unflown; the flight above is still required.
