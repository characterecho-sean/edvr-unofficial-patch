# The cockpit HUD after the upscale: design

## Status

- **State: the census retired 2026-09-29 (code removed, 7027e549).**
  `advanced.hud_census` and hud_layer_census.cpp are deleted; Sean chose
  deletion over a compile-out switch, so there is no diagnostics build flag.
  Phase 0 below and the flights that ran it (the last, 0229c358) are history.
  The code is at `7027e549^`.

- **State:** combined Frontier branch `codex/dlss-performance-review` stays
  separate from main. Selector unwind `967f0519` is installed and verified.
  Prior `c668f83f` performance flight is verified; Sean reports "Much better."
  Earlier `7aaaf39c` flight `033435`/dump `033645` confirms both cockpit
  holograms in final crisp composition; Sean says they look great. Holo panels,
  flight HUD, target sprite and eight generic families retain their HDR layer.
  The user requested unwinding the mistaken
  VS71DD8B8B09060A81/PS2D037A047171BF3B admission: it paints world-space
  brackets, while scanner-rim yellow <> remains unidentified. That admission,
  PS observation and dedicated fixture are removed; pre-existing world-marker
  depth/motion, hologram remaps and general final capture are preserved.
  Frontier `0229c358` flew with diagnostics off/on: Sean says HUD looked good.
  This is a qualitative visual check, not measured bloom parity. Armed HDR
  shading median .126 ms/eye; machinery .962 ms/eye with seed .612. Joint
  guarded seed optimization is installed; see the combined performance review.
- **Open:** scanner-rim yellow <> shader remains unidentified and is deferred
  to another agent at the user's request. Earlier final overlays changed world
  brackets and nearby pixels; that did not prove ship mesh admission. Sphere
  depth reads originate in Elite; the coordinate remap passes 927 WARP/RTX
  checks each and the full build. Current flight confirms model delivery, with
  first right crop/overview explicitly passthrough. Controlled alpha/occlusion
  parity remains unqualified.
- **Goal:** composite the cockpit HUD after the upscale, at output resolution,
  out of DLSS/FSR history. That means the holo panels, the flight HUD and the
  target sprite. It should be as sharp at HMD Quality 0.5-0.75 as at 1.0. Menus
  already leave the upscaler through `fix.ui_quality`'s layer.
- **Why:** field reports on 2026-09-27.
  - One user's HUD is soft and smears below HMD Quality 1.0. Their rc.2
    logs show a DLSS input of 65-85% with `fix.ui_quality` off; 1.0
    "mostly" fixes it.
  - Another user found "the cockpit panel is not excluded from the AA
    pass". That is correct and by design: the layer refuses these
    families as `kHdrTarget` (`ui_layer_math.h:913`).
- **Prior art:** this is crisp-ui-handoff.md's parked form of Design A (lines
  184-191: "PARKED, not declined"), with one change. It re-issues the game's
  own tonemap draw instead of transcribing it.
- **Next step:** selector unwind validated, pushed and installed on Frontier.
  Keep scanner identification deferred to the other agent. See
  `dlss-performance-review-2026-09-28.md` for the confirmed CPU waits and
  retained optimizations. Halo/occlusion parity still needs pixel evidence.

- **Latest journal entry (2026-09-29, built, not flown):** the hologram
  restore fence lifts at the frame boundary (Review B1); ui_holo_test now
  1059 checks on WARP (927 in the entry the Open bullet's figure came from).
- **2026-09-30 (built, not flown):** the re-issue's eye identity now follows the
  eye through the game's post pass, so the station menu's frosted base is taken
  after the HUD (ui-layer-2026-09-23.md, "2026-09-30"); its Limits name the HDR-phase inversion.
- **Design background** (the game's tonemap draw, its variants, exposure,
  bloom; measured facts): "Status detail" below, verbatim.
- **Ruled out:** "Ruled out (do not re-propose)" below; the phase journals
  after "Decisions" hold the flights.

## Status detail (moved out of Status 2026-09-29)

Moved verbatim out of the Status block (the design background bullets); the
summary above points here.

- **Tonemap** (vs `2D78DC3FD2C0C543` / ps `99C21CEB7A699821`), MEASURED
  (eye_tonemap_snapshot.h:87,139,170-201):
  - a 3-vertex full-screen triangle, unblended, no DSV;
  - VS t0 is the scalar exposure (`R32`); PS t0 the colour LUT (3D); PS
    t1 the HDR source; PS b2 at least 256 bytes;
  - the output is the RGBA8 eye.
- **Variants:** under EDHM and the settings tiers the tone PS varies. The
  flat-AA arc's tone admission identifies them by VS set and per-PS HDR slot
  (design-flat-temporal-aa-2026-09-23.md sections 63-65). Reuse that rather
  than one hash.
- **Exposure:** `fix.share_exposure` copies eye 0's exposure over eye 1's
  (exposure_fix.h). A re-issue that binds the game's own exposure SRV inherits
  it.
- **Bloom:** BELIEVED baked into the HDR source by the composite pass. It is
  computed from the HDR target, so it includes the HUD. That is the HUD's halo,
  measured in the input frame of eye dump 123118.

## Ruled out (do not re-propose)

- **Taking the families into the 8-bit layer as drawn:** they write HDR
  radiance before exposure and tonemap, so the layer would lose both. This
  is the reason the code gives for kHdrTarget.
- **Rebuilding the deferred UI replay** (retired 48ad7689). It used
  parallel capture and command lists, D3DReflect on shaders with no RDEF
  chunk (reflection reports zero bindings; edhm-black-cockpit-2026-09-15.md),
  and alias-matched submits. It logged captured=0 on every flown rig.
  Use the layer's live redirect, which flies.
- **Transcribing the tonemap into EDVR HLSL:** it drifts from EDHM and the
  settings-tier variants. Re-issue the game's draw instead.
- **Drawing the HUD in both the HDR image (for bloom) and the layer:**
  additive elements would count twice.
- **Moving the upscale before the tonemap** (HDR DLSS, with the HUD
  blended in HDR at output size): exact, but it moves the whole temporal
  pass. Out of scope. Name it in the design review if parity fails.

## The design

1. **An HDR HUD layer** per eye: `R16G16B16A16_FLOAT` at the door's output
   size x the `fix.ui_quality` target (100 when that key is off). It holds
   premultiplied HUD radiance plus coverage, in the 8-bit layer's own
   alpha convention (read `uiLayerComposite` for it). At 4074x3938 that is
   128 MB per eye at 100 and 200 MB at 125; at a Quest 3's 2564x2460, 50
   MB. Clear it per eye-frame.
2. **Take the families:** with the new key on, `uiLayerDecide` routes
   kHolo, kFlightHud and kSprite, and any other HDR-target composite of a
   learned surface (see "The three families"), to the HDR layer instead
   of kHdrTarget.
   They get the same remap, jitter cancel, seeded depth (they test GEQUAL
   against the scene pair), refusals and census, and the `!layered` skips
   of ui_depth, the hologram pass, the reactive mask and screen motion.
   Confirm the "tone separation" re-issue in the vscreen.cpp:3677 comment
   is covered too. The flight HUD's t0 depth read is eye-sized and
   NDC-addressed, so it survives the remap. The after-UI rule's eye-sized
   post-pass test must not see it, because it is a named family.
3. **Tonemap re-issue**, in frame at the game's own tonemap draw (tone
   admission identifies it), once per eye:
   - bind the same VS/PS, b2, LUT and exposure;
   - put the HDR layer's SRV in the admitted HDR slot;
   - render into the eye's 8-bit UI layer, RGB only, at the layer's
     viewport;
   - one EDVR pass then writes the HUD's coverage into that layer's alpha;
   - restore everything through the vScreen*Raw entry points.

   The post-tonemap menu draws then land on top, in game order. The
   existing door composite shows both. There is no new composite and no
   separate LDR HUD image.
4. **No bloom on the HUD:** its halo goes (the look changes). SMAA does
   not apply either; at output size x 100-125 the edges need less. If
   Sean wants the glow, a later phase blurs the tonemapped HUD layer.
5. **Parity:** stock computes T(F(1-a) + L); the layer gives T(F)(1-a) +
   T(L). The two agree where the HUD is opaque or the background dark.
   They differ where a translucent HUD crosses a bright background (a sun,
   a lit station). Gate G-F measures it before Phase 1 ships.

## Phase 0: census gates (no rendering change)

One flight. Cockpit in space, then a station with a target locked, at HMD
Quality 0.75 with DLSS. Each gate is a `hud layer census:` line:
- **G-A, the families:** per family per frame, the target resource and
  format, blend, depth/stencil state, SRV sizes, and the draw index
  relative to the composite pass, the tonemap and the door. This settles
  the sprite's target and gives the flight HUD's draws per frame.
- **G-B, the tonemap:** the admitted variant (VS/PS), its SRVs (exposure,
  LUT, HDR), b2's size, and whether its HDR SRV is the resource the HUD
  drew into.
- **G-C, jitter:** each family's projection rows against the scene's
  jittered ones (G4 per family).
- **G-D, occlusion:** each family's pixels rejected by its GEQUAL test
  (occlusion queries, test on against off). A large share means the
  seeded depth's input-resolution edges show.
- **G-E, exposure:** HUD display brightness against the exposure scalar,
  across a bright and a dark scene.
- **G-F, parity:** with the HDR target and the HUD's own contribution
  captured for one eye-run frame, the error |T(F(1-a)+L) -
  (T(F)(1-a)+T(L))| on HUD pixels (p50/p99, in 8-bit steps), computed
  offline or in a diagnostic pass.

## Phases

1. The holo panels only, behind the key (default off). Log lines, every
   30 s:
   - taken, left and refused per family, with the reason;
   - HDR layer size and memory;
   - tonemap re-issues and declines, with the variant.
   GPU time for the HUD draws at layer size and for the re-issue goes into
   the existing layer timing lines.

   Rig (extend `ui_quality_test`): a model of HUD over background with a
   stand-in tonemap. Opaque, additive over dark and translucent over
   bright match stock within the tolerance the gate chose, and fail with
   the take removed.

   Flight: cockpit at HMD 0.5 and 0.75 with DLSS. The panels should read
   as sharp as at 1.0, with no brightness step against the pass-off frame
   (the G-F budget) and the census clean.
2. The flight HUD and the target sprite, once G-A has settled the sprite's
   target.
3. Optional: the hologram families (radar contacts, the ship and target
   holograms, icons: the hologram pass's eleven). They are in the HDR
   target too. Taking them would retire most of that pass.
4. Optional: the glow (see Decisions).

## Risks

- A brightness step from parity (G-F).
- Occlusion edges at input resolution (G-D).
- A family without jitter, which the cancel would shift (G-C).
- The PS hashes under EDHM: families are keyed by VS, variants by the tone
  admission tables.
- Memory, above.
- GPU: the panels shade 4x the pixels at 2x per axis (measure).
- VR-specific: the layer seeds depth per eye, and the HUD must not pick up
  the wrong eye (the layer's eye check, "0 SWAPPED", covers it).

## Decisions

For Sean, before Phase 1 -- ANSWERED 2026-09-27:
1. Accept losing the HUD's bloom halo, at least initially? YES (the G-F
   numbers sized it: a few dozen pixels a frame in the bright-translucent
   regime; the Phase 4 blur is the way back if the edges read wrong).
2. The key. Suggested: `fix.crisp_hud = off | on`, named for what the
   player gets, which follows `fix.ui_quality`'s size target and arms the
   layer at 100 when that key is off. Or fold it into `fix.ui_quality`.
   SEAN: folded -- the holo take is part of fix.ui_quality (no key of its
   own).
3. Scope: the three families (Phases 1-2), or the hologram families too?
   AS SUGGESTED: three families; the holograms stay Phase 3-optional.
4. The memory budget: +128-200 MB per eye on a Crystal-class output.
   ACCEPTED (the configure and layer lines report the actual numbers).
5. Default: off until flown on Pimax and Quest. STANDS -- fix.ui_quality
   defaults off, so the take defaults off with it. SUPERSEDED 2026-09-29:
   Sean ships fix.ui_quality at 100, so the take is on by default (with
   fix.temporal_aa on) before those flights; ui-layer-2026-09-23.md, journal
   2026-09-29.

## Phase 0 built, 2026-09-27

The census is code now, on branch `kimi/crisp-hud-census` (held off main
until the feature flies and verifies on the Steam install). Full build
green, every gate including the new `hud_parity` self-test.

- **The instrument:** `advanced.hud_census = off | on` (default off, live),
  a new module `src/d3d11/hud_layer_census.{h,cpp}` hooked into vscreen's
  eye-draw branch INDEPENDENTLY of `uiLayerLive()` -- the design flight has
  `fix.ui_quality` off, and the family naming runs only with the layer
  live. Unarmed cost is one bool per eye draw. G-A family state lines on
  first-seen/change plus 30-second window lines with draws-per-frame; G-B
  tonemap variant lines (structure-first recognition, exact hashes logged,
  so an EDHM swap names itself); G-C jitter verdicts on change; G-D
  depth-rejected shares; G-E exposure + HUD-region HDR luma at 1 Hz. All
  lines prefixed `hud layer census:`.
- **G-D's shape:** an occlusion-query pair per sampled family draw, the
  game's own GEQUAL test with ALL writes masked against depth-and-stencil
  off, both re-issued with NO colour target, full OM save/restore, queries
  never waited on. It declines on predication, on PS UAVs, and -- a case
  the design missed -- while ANY game query is open on the context (a
  re-issue inside the game's own occlusion bracket would feed its counter
  and change what it draws a frame later; tracked from the Begin/End
  hooks).
- **G-F is offline:** `tools/hud_parity.py` reads one F10 eye-run ledger's
  `panels_<stamp>.bin` (EyePanelSnapshot already captures the HDR target
  before AND after each holo draw -- F and F(1-a)+L) and `tonemap_<stamp>.bin`
  (exposure, LUT, HDR/output crops), replays T empirically from the captured
  HDR->output pairs, and prints p50/p99/max of the parity error in 8-bit
  steps, with the verdict gated on the bright-translucent regime. `--self-test`
  green and gated in build.bat.
- **Corrections to this doc from the build:**
  - ruled out: "the composite pass `953C8123AD8DC13B`, believed to add bloom"
    as a cockpit anchor -- every reference in this repo names that hash the
    FSS scanner-body composite (edvr.ini, fss_probe.h, crisp-ui-handoff.md).
    The census anchors ordering on the tonemap draw and logs any cockpit
    sighting of the FSS hash to settle it.
  - The family clip rows are VS cb0 rows 4..7, not 0..3
    (flat_projection_recipes.h); G-C reads 64 bytes at offset 64 and votes on
    the centre terms (m02/m12), which are all the jitter moves.
  - The tonemap VS already varies in the wild: EDHM flies vs
    `642017A6FEDAE0E8` with the same PS (edhm-black-cockpit-2026-09-15.md).
- **Flight checklist (Steam):** install (`python tools\install_edvr.py
  --target steam`), set `advanced.hud_census = on`, `fix.ui_quality` off,
  HMD Quality 0.75 with DLSS. Cockpit in space (2 min), then a station
  with a target locked (2 min), one bright scene and one dark (G-E); press
  the eye-dump hotkey once with panels on screen (G-F). After:
  `python tools\edvr_log.py --target steam --expect-build HEAD`, then
  `--grep "hud layer census:"`, and `python tools\hud_parity.py <ledger
  dir> --verbose`.

## Phase 0, flight 1, 2026-09-27 (Steam, fc89d59d)

Short flight (~2 min: menu, cockpit, the FSS scanner mid-flight, a
HUD-present stretch of ~270 frames at 2600x2514 = HMD Quality 0.75, DLSS
on, EDHM installed and active). Build stamp verified by
`edvr_log.py --expect-build HEAD` before any counter was read. 9,596
`hud layer census:` lines harvested; the per-gate verdicts:

- **G-A SETTLED.** Holo: 22.0 draws/frame (11/eye), the lit HDR target
  (R11G11B10_FLOAT), premultiplied over, GEQUAL depth with no depth write
  -- and a STENCIL WRITE the doc's table missed (ref/write 0x04, pass
  REPLACE: Phase 1's take needs the write-back machinery for these).
  Flight HUD: 4.8 draws/frame (4..6; was UNKNOWN), same HDR target,
  GEQUAL, stencil off; t0 = the eye-sized R32_TYPELESS depth and t1 = the
  256x256 grain LUT, both as the doc's table said. Sprite: 2.0 draws/frame
  (1/eye with a target) -- the target dispute is SETTLED for the lit HDR
  target (R11G11B10_FLOAT, the same two resources as the other families;
  ui_depth.cpp:116-131 right, crisp-ui-handoff.md:174's RGB10A2_TYPELESS
  refuted on this config) -- but its state is NOT the table's GEQUAL:
  depth test off with write-all, stencil on (0x05, GREATER), consistent
  with the VS forcing device Z to 1. Ordering holds: families at ordinals
  503..2684, the tonemap at 767..2687, always after. The bloom composite
  953C8123AD8DC13B was never seen in cockpit (0 sightings): MEASURED now
  that this hash is the FSS scanner-body composite; where bloom lives in a
  cockpit frame stays BELIEVED and un-hashed.
- **G-B SETTLED, with the variant named.** The measured pair (vs
  2D78DC3FD2C0C543 / ps 99C21CEB7A699821) flew as a full structural match:
  exposure R32 at VS t0, 3D LUT at PS t0, HDR source at PS t1 = the
  families' own target resource (identity join 442/442 in the HUD window),
  b2 = 272 bytes. The EDHM swap flew too (vs 642017A6FEDAE0E8, same PS) --
  and it binds NO exposure at VS t0, so a Phase 1 re-issue must bind the
  admitted draw's own SRVs, never a remembered exposure (where EDHM's
  exposure lives is an open question, next to the G-C rework). Two
  menu-size composites named themselves shape-only and were never
  followed; the structure-first recognition did its job.
- **G-C NOT SETTLED -- instrument gap.** All three families' cb0 rows
  4..7 are a composed model-view transform (dense rotation-like rows;
  row 2 = [0 0 0 0.025]), not the bare projection, so the centre-term
  test has nothing to compare (0 jittered, 0 unjittered, n=3552
  not-scene). "All three carry the eye's jittered projection" stays
  BELIEVED. Next instrument: capture the whole cb0 for offline
  factorisation, or vote the per-eye row deltas against the per-eye
  jitter delta; the projection may not live in cb0 at all.
- **G-D NO DATA -- instrument gap.** Every selected pair declined (2,290)
  and no result ever polled. Near-certain mechanism: EDVR's own gpu_span
  TIMESTAMP_DISJOINT query is open across the frame's draw sections
  (gpu_span_d3d11.cpp:75) and the guard declines on ANY open query, though
  a disjoint/timestamp counts no samples and could not be fed by the
  re-issue; game predication is the other candidate. Next instrument:
  split the decline counter by reason, and make the guard type-aware
  (decline only for occlusion-family queries and predication).
- **G-E SETTLED directionally.** The tonemap's own VS t0 exposure tracked
  the scene: ~600 in the bright stretch down to ~44-51 in the dark one.
  The HUD-region crop luma follows the background more than the HUD
  (0.15-0.23 bright, 0.003-0.012 dark), so "HUD display brightness"
  proper stays an offline read; the gate's deliverable is that exposure
  is a per-frame scalar the re-issue inherits by binding the admitted
  draw's own SRV -- with the EDHM caveat above.
- **G-F NO DATA.** The eye-run hotkey (INSERT) was never pressed; no
  ledger armed, no panels_/tonemap_ bins. Re-fly item.

Re-fly notes: keep the cockpit HUD up for one full 30 s window (the FSS
scanner replaced it mid-flight this time), fly one bright and one dark
scene, and press INSERT once with holo panels on screen for G-F.

## Phase 0.1 instrument, 2026-09-27 (6798b6de)

Flight 1's two instrument gaps are fixed on the branch: G-D's open-query
guard is type-aware (it declines only while a sample-counting query --
occlusion, stream-out or pipeline statistics -- is open; gpu_span's
frame-wide TIMESTAMP_DISJOINT no longer trips it), declines are counted
by reason on their own window line, and a stream-out-bound decline was
added beside the PS UAV one. G-C reads the whole VS cb0 (up to 16 rows)
and votes each 4-row quad for bare-projection structure; a family whose
projection lives outside cb0 dumps every row once per eye for offline
factorisation. Installed to Steam as v0.18.0-rc.2-50-g6798b6de; awaiting
flight 2 (same profile as flight 1, plus one INSERT press with holo
panels on screen for G-F).

## Phase 0, flight 2, 2026-09-27 (Steam, 6798b6de, two sessions)

Both logs verified build 6798b6de. Sessions 12:21 and 12:24; five eye-run
ledgers at 12:30:21..12:31:04. ~19.2k census lines harvested.

- **G-D: the guard fix worked; the log cap ate the results.** Zero
  declines in every window (the type-aware guard passes gpu_span's
  disjoint), all three families' game depth states were cloned and pairs
  began -- and then the gfx log hit its 4 MB cap at 12:26:50, seven
  seconds before the first window carrying G-D results would print. The
  cap's cause is the census's own G-A flood: a single last-fingerprint
  slot re-logged every panel of every frame (~12 distinct holo interface
  surfaces cycle through one family's slot), ~9.7k lines in two minutes
  of cockpit. Fixed as Phase 0.2: first-seen-per-session fingerprint sets
  (64 per family, with a table-full note). G-D data: still none; flight 3
  gets it by holding the cockpit for one 30 s window.
- **G-C: cb0 ruled out; the composed matrix is shared.** The wide read
  dumped all of cb0: rows 0..3 are constants ([1 1 1 1], [0 0 0 0],
  [16 16 0 0], [16 16 0 0] -- panel parameters, not a projection), rows
  4..7 a composed transform, and MEASURED near-identical per eye across
  all three families (row 4 agrees to ~3 decimals between holo, flight
  HUD and sprite): one shared per-eye view-projection, the
  family-specific part elsewhere. Jitter inside a composed matrix is not
  separable by row inspection; the flight-2 ledgers captured the
  families' VS b0/b1/b2 (pool\draws_<stamp>.bin), so factorisation
  against the scene camera is offline work -- no re-fly needed. "The
  families carry the eye's jittered projection" stays BELIEVED until
  then; Phase 1's jitter cancel keeps its G-C gate.
- **G-F: measured.** Five ledgers, ~440k HUD pixels pooled. err p50 0.00,
  p99 0.00 8-bit steps: stock and the layer agree except in the predicted
  regime. In it (0<a<1 over background luma > 1): 5-40 pixels a frame
  (0.0-0.1%), restricted p99 40-113 steps; the stamp with the most
  in-regime pixels (123104, 40 of 221k) fails the default budget (113 vs
  2). Caveats measured alongside: every panel draw is INSTANCED, so the
  a/L recovery used the luminance fallback (per-pixel a is heuristic; the
  cross-check is by-construction there), and the empirical T fit residual
  ran p99 1.5-2.5 steps. Physical reading: a layer-composited HUD differs
  from stock only where translucent glass crosses a bright background, a
  few dozen pixels a frame, and there stock is brighter -- the halo,
  quantified. That is Decisions 1's price with numbers; the Phase 4 blur
  is the way back if those edges read wrong in flight.
- **G-A/G-B consistent across both sessions and both render sizes flown**
  (2600x2514 and 3000x2901): holo 22.0 draws/f with the 0x04 stencil
  write, sprite 2.0/f depthless with stencil 0x05, flight HUD ~5/f with
  the eye-sized R32 depth at t0; tonemap 1.00/frame/eye in session 1,
  five variants named (the same five), bloom composite 953C8123AD8DC13B
  never sighted in cockpit again.
- **G-E:** exposure tracked 11.3..115 across the two sessions' scenes;
  the HUD-region crop still mixes scene and HUD (by design).

## Phase 0, flight 3, 2026-09-27 (Steam, 4db05397)

One session, ~4 min, build verified. The Phase 0.2 dedupe held: 143 census
lines for the whole session (flight 2 spent 9.7k in two minutes), the log
cap never approached, and the G-D windows printed.

- **G-D SETTLED: the depth test rejects nothing.** Per family per eye the
  occlusion pair's on/off sample counts were EQUAL (holo eye 0: 33,123,678
  of 33,123,678 passed, 1,989 pairs; flight HUD eye 0 across the second
  window: 1,282,658,327 of 1,282,658,328 -- ONE sample rejected in 1.28
  billion). The doc's "large share means the seeded depth's
  input-resolution edges show" is measured absent: occlusion is ~0%, so
  Phase 1's seeded depth is a correctness item (the GEQUAL test exists and
  runs), not an edge-quality one. Two qualifications, both measured: the
  sprite's own state is depth-off, so its pair A equals B by construction
  and its line carries no gate; and the flight exercised cockpit-in-space
  and station-with-target, not a panel buried behind the dashboard at an
  extreme look-down. The ring-full declines (14-19k per window) are the
  8-pair ring throttling the 12-pair/frame budget -- sampling only; the
  accumulated counts make the 0.0% robust.
- **G-A, final numbers:** holo steady at 22..24 draws/frame (11-12/eye)
  across every flight and both render sizes; the flight HUD is
  content-dependent, 4.8 draws/f in quiet flight up to 54-56/f (27/eye) in
  the busy station scene; the sprite 2..6/f with a target. The doc's
  flight-HUD UNKNOWN is a measured range now.
- **G-B:** the identity join held 100% whenever families were present
  (4,282/4,282 and 4,526/4,526 tonemaps' HDR SRV is the families' target);
  bloom composite 953C8123AD8DC13B never sighted on a third flight; the
  five variants are the same five every session.
- **G-C unchanged:** no cb0 quad is the bare projection (n=16k+ per family
  per window, all not-scene); the dumps are consistent with flight 2's and
  add that eye 0/eye 1 differ in rotation while the w column differs per
  family -- shared view rotation plus per-family translation. The
  factorisation against the scene camera stays the named offline path
  (flight 2's draws_*.bin carry the families' VS b0/b1/b2).
- **G-E:** exposure 42..625 across this flight's scenes, same behaviour.

Phase 0 closes with one open item: G-C's factorisation, which needs no
flight. Every other gate is answered; the Decisions are unblocked.

## Phase 0, G-C settled offline, 2026-09-27 (flight-2 ledgers)

The factorisation ran on the five flight-2 ledgers' EyeDrawSnapshot
captures (drawstate_<stamp>.bin; extraction and cross-checks in
analysis/gc_results.md, git-ignored scratch; the raw bins stay in the
Steam install's edvr_logs\pool). No flight was needed.

- **Verdict: jitter CARRIED -- all three families, both eyes, all five
  ledgers, every one of 7,039 draws, by construction.** Every family draw
  binds a 336-row VS b1 whose rows 270..273 are ONE global per-eye camera
  matrix (within-frame spread across draws: exactly 0.0; layout as
  object_probe.cpp:956-958 documents, eye origin at row 275). The family's
  clip transform (cb0 rows 4..7) equals that matrix in its 3x3 part
  BIT-EXACT in every sampled draw; the w column is the same matrix applied
  to a per-panel translation. So the HUD projects with the engine-wide
  per-eye jittered transform, and the doc's BELIEVED "all three carry the
  eye's jittered projection" is MEASURED. Phase 1's cancel-exactly-once is
  correct as designed.
- The camera's centre terms oscillate per frame with EDVR's exact 8-phase
  Halton jitter (m12 slope = 2/2901 exactly; m02 at 94-99% of 2/3000,
  small per-phase residuals, likely the game-side tangent round-trip),
  which is also the proof the matrix is the live jittered one and not a
  stale copy. The in-sim census's gc dumps from flights 2 and 3 land
  exactly on the ledger per-frame values -- dump == ledger == one
  transform, and the layer's eye mapping is confirmed (0 SWAPPED never
  fired).
- Caveats carried into Phase 1 (recorded, not blocking): the absolute
  jitter phase cannot be anchored from a ledger (ledger frame != temporal
  frameCounter) and the game-applied x jitter runs a few percent under
  the naive -2*jx/w slope, so a cancel driven by EDVR's own s->shift could
  leave a few-percent-of-a-pixel residual -- validate visually in the
  Phase 1 flight. "b1 is the scene camera" rests on the documented layout
  plus the jitter fingerprint; a direct scene-side confirmation, if ever
  wanted, is one re-fly with advanced.eye_depth_capture on, comparing the
  scene pair's b1 rows 270..273.

## Phase 1 built, 2026-09-27 (f032e024)

The design's Phase 1 is code, on the branch and installed to Steam
(v0.18.0-rc.2-56-gf032e024). Built per the design with the Phase 0
measurements folded in; Sean folded the key into fix.ui_quality (decision
2) -- the take arms with the layer at 100/125 and is off by default with
it (decision 5 stands).

- **The take:** a holo draw into the lit HDR target routes to the eye's
  HDR layer (R16G16B16A16_FLOAT, door size x target) through the LDR
  take's own path -- same remap, same jitter cancel (G-C: the families
  carry the jitter, so the cancel is exact), same seeded depth-stencil
  (G-D measured rejection ~0%), and the measured 0x04 stencil write keeps
  landing in the game's own buffer through the existing colourless
  write-back. The flight HUD and the sprite refuse kHdrTarget as before
  (Phase 2).
- **The re-issue:** the tonemap draw is admitted structurally
  (tonemap_admit.h, the census's G-B recognition factored out and
  shared), once per eye per frame, right after its own issue: same VS/PS,
  b2, samplers, the admitted draw's own exposure and LUT (the EDHM swap
  binds no exposure at VS t0 -- nothing is re-bound there), the HDR layer
  at the admitted per-PS HDR slot, rendering RGB-only into the 8-bit
  layer at the layer viewport; one EDVR pass then writes the HDR layer's
  coverage into the 8-bit layer's alpha. Menus after the tonemap land on
  top in game order; the door composite is untouched. The "tone
  separation" case is the once-per-eye guard (a second admitted tonemap
  for an eye is counted, never re-issued).
- **Declines, all counted and named once:** no HDR slot (unknown PS --
  never guessed), no content, layer busy (an LDR draw already holds the
  frame -- the ordering guard), second tonemap, size mismatch, state
  drift, PS UAV, layer failed. A failure stands only the HDR path down;
  the LDR take is untouched; off means exactly stock.
- **The rig:** ui_quality_test's parity block models stock T(F(1-a)+L)
  against the layer's T(F)(1-a)+T(L) through two stand-in tonemaps:
  opaque/uncovered bit-exact, dark within the G-F budget (2 steps), the
  translucent-over-bright regime bounded by flight 2's measured ceiling
  (113 steps), and proven sensitive to a take-removed mutant.
- **Phase 1 flight protocol:** fix.ui_quality = 100, HMD Quality 0.5 then
  0.75 with DLSS, cockpit with the panels up (a station with a target
  locked for the busy scene). The panels should read as sharp as at 1.0,
  with no brightness step against pass-off (the G-F budget) and no halo
  (accepted). After: `python tools\edvr_log.py --target steam
  --expect-build HEAD`, then the "crisp hud" and "ui quality" lines:
  taken 22-24/frame, re-issues 2.00/frame, declines none, the HDR layer's
  size/memory, the route GPU times. Validate the jitter cancel visually
  (the G-C caveat: absolute phase unanchored, x slope at 94-99%).

## Phase 1 flight 1, 2026-09-27 (Steam, f032e024): the menu regression

Cockpit side measured clean: 12.41 then 23.24 holo draws/frame taken,
2.00 re-issues and coverage passes a frame, 0 declines, 0 HDR content
lost, 0 composites refused. Sean: "cockpit looks good".

The regression: the ui menus. The family census says the menus were NEVER
taken this flight -- 0 redirected in every window. Two measured
populations:

- In the cockpit: the in-flight menu composite (station services, the
  escape menu) draws vs A888D51024D9798E / ps 015EF9349EC097E8 -- the
  TINTED variant, documented in ui_depth.cpp:105-115 (three variants, nine
  disassembly lines apart, none in the sampling) but MISSING from the
  family rule's pair list (kUiPanelPs had only the two main-menu PSes
  since 2026-09-23). With no learned surface bound at recognition, the
  family was never found: "no learned surface, pixel shader not known",
  ~5,200 draws a window. This gap predates the branch -- the recognition
  is identical on main -- but ui_quality defaults off, so nobody had flown
  the menu take against the tinted variant.
- The main-menu/loading untinted panels (ps 9107E72CB016CC02, in the list)
  were recognized but refused "eye unknown" (uiDepthEyeOfTarget can't name
  their target's eye there) -- unchanged behaviour, separate question.

The regression's mechanism: with the menus left in the eye and the holo
panels now composited over the finished eye at the door, an open in-flight
menu sits UNDER the cockpit panels. Stock order (panels under menus) held
before because the panels never left the eye.

The fix (43ab5364): the tinted and cheap variants join kUiPanelPs -- the
pair route exists exactly for "no learned surface bound" -- so the
in-flight menu takes, lands in the layer AFTER the re-issue (game order:
menus draw post-tonemap), and sits over the panels again, now at layer
sharpness. The rig's family-rule fixture moved to the new expectation.
Built green, installed to Steam (v0.18.0-rc.2-58-g43ab5364). VERIFY with
a docked menu open: the menu over the panels and sharp; the log's family
line should show "decided as the menu panel: N redirected" with the tinted
PS named, and the crisp hud line clean.

## The main-menu regression, 2026-09-27 evening (84690880)

Sean after the morning install: "Main menu is still being drawn before
the AA pass" -- and reported all UI menus had taken the layer before.
Tonight's log (43ab5364): every menu draw left with "eye unknown", 0
redirected in every window. The diff against rc.2 is additive in the
layer and nil in ui_depth, and the game has been build 332841 since
2026-09-19, so the mechanism was in the branch: the crisp tonemap
admission asked uiDepthEyeOfTarget for the tonemap's LDR output every
frame, and that ask REGISTERS in ui_depth's per-frame eye table (first
target of a shape = left, second = right, the third gets "no eye"). The
main menu runs at 2000x1934 with the menu composite in its own buffer, so
the tonemap's two outputs held the shape's two slots before the menu's
target ever asked -- "eye unknown" on every menu draw, forever. In-flight
the composite writes the tonemap's own output, which is why the cockpit
never noticed.

The fix: the admission names the eye from the admitted draw's own HDR
source instead -- it IS an eye's HDR target and the holo take records
which this frame, exact, no table. The census's two observer lookups get
uiDepthEyeOfTargetReadOnly (never registers); the layer's decide keeps
the registering form, as the classifier would. The tinted/cheap pair
admission from the morning stands. Built green, on Steam as
v0.18.0-rc.2-60-g84690880.

Verify (one launch): the main menu sharp (taken, not before the AA pass)
-- the log's "left in the game's frame" line should no longer list menu
panel with "eye unknown"; then docked, a menu open over the panels: menus
over the panels, both sharp; the crisp hud line clean.

## Phase 1 flight 2, 2026-09-28 morning (84690880): the tier variant

Menus fixed and sharp (last night's eye-table fix holds). But the cockpit
panels were gone at HMD Quality 0.50 + ui_quality 125: the tier flies its
own tonemap PS (D0A16B9E55BF22CC), the admission's per-PS slot table knew
only the measured 0.75-tier one, every re-issue declined kNoHdrSlot, and
every taken panel vanished (4,060/4,062 draws; the window lines named it:
"0.00 re-issues, 4,060 HDR layers' content never reached a tonemap").
Sean: the panels gone; the radar, the ship hologram and the target
hologram still present (the hologram pass's families, never taken) -- the
ship without its shields (the shield ring is a holo-panel draw, taken and
lost with them). Exactly the families split the take makes.

The fix (09baba69): the admission finds the HDR source by IDENTITY -- the
PS slot whose 2D view reads an eye's HDR target this frame, which the holo
take records -- so the tier and EDHM variants need no table entry. And the
failure shape hardened: a draw with fresh content but no readable slot
stands the crisp path down to stock (named once, counted) instead of
losing the HUD for a session. Phase 0's census never saw this PS because
flights 1-3 ran 0.75 only -- the tier gap is recorded in the G-B entry's
risks now.

On Sean's refactor question (HUD and ui_depth sharing a path): the eye
table regression (observers registering) argues for exactly that direction
-- the admission now answers its own questions by identity instead. A
fuller merge of the two passes' recognition is real work and is NOT
folded into this regression fix; noted for the Phase 2 review.

## The review round, 2026-09-28 (d9ead2e8 + cfa72f33)

reviews/crisp-hud-census-review-2026-09-28.md (against 09baba69) found
seven P2s; all fixed on the branch:

- R1/R2 (production failure paths): the missing-consumer stand-down now
  requires the genuine full-shape tonemap whose OUTPUT eye has
  outstanding content (SMAA and the post passes are tonemap-shaped and
  were one flight from standing the feature down), with the real backstop
  a 30-consecutive-frame publication deadline per eye; and the crisp take
  now establishes its whole dependency set (HDR layer, 8-bit layer, RGB
  blend, coverage shaders, deferred context) at the FIRST take -- a
  failure refuses the take (stock) instead of dropping the taken HUD.
- R5: a crisp multiply refuses before redirect (no HDR transmittance
  route; nothing measured uses it), with rig coverage.
- R6: the census joins the draw-gate subscriber expression.
- R7: the open-query set is tracked armed or not; pre-arm brackets block
  G-D until they close.
- R3/R4 (parity evidence): stock-unchanged covered pixels can no longer
  be omitted from the gate (the equality fixture now fails at 32.9 steps,
  as it should); pairing must be proven (stamp + frame + HDR-resource
  identity, unique) or the verdict is INVALID, never a PASS; the T-fit
  and cross-check residuals carry ceilings (4 steps / 0.25 HDR units)
  that also land INVALID. --allow-mismatched measures under a banner,
  never a PASS.

The review's noted non-findings stand: the lost halo and the nonlinear
translucent difference are the accepted trade; the menu eye-table fix and
the identity admission were already in. Full build green both halves
(265 rig checks, parity self-test with the new negatives). Awaiting the
consolidated verification flight: the 0.50/0.75 tiers, ui_quality 100/125,
menus over cockpit panels, and feature re-arming (a ui_quality toggle
mid-flight).

## Phase 2 built, 2026-09-28 (b2c6d6e8 + merge 864c1c1c)

The flight HUD and the target sprite take the HDR layer through the same
crispHdr gate as the holo panels (the doc's Phase 2, gated on Phase 0's
measurements). Verified family-agnostic rather than assumed: the take
branches only on g_draw.hdr; the flight HUD's t0 scene-depth read is
NDC-derived and survives the remap; the sprite's measured state (depth
test OFF -- uiLayerDsEffect requires depthEnable for depthWrite, so stock
performs no depth write; stencil test 0x01 / write 0x05) is covered by the
on-demand per-bit stencil seed and the existing colourless write-back, the
path the holo 0x04 stencil write flew with. hud_grain (deleted 2026-09-29,
0467e706) and target_indicator brackets nest as before. Rig: 274 checks, 0
failures.

Then origin/main merged into the branch (the Coriolis-blur arc and the
rc-since-rc2 review round; two conflicts, both in the expected places:
uiLayerNoteOther's new excluded/panelSized parameters from the F4 fix
combined with the crisp-pending guard at the call site, and the crisp
section beside the new signature). Merged build green (config contract
264/264), installed to Steam as v0.18.0-rc.3-29-g864c1c1c.

Phase 2 flight: HMD 0.5 and 0.75 with DLSS, ui_quality = 100. Eyeball: the
flight HUD's cockpit fade against a known occluder (its t0 read is
scale-free by construction); the target sprite's shape and position with a
target locked (never occluded by construction); no brightness step (the
G-F budget); the target HOLOGRAM unchanged (Phase 3, not taken). The log:
"crisp hud" sums all three families (~28-80 draws a frame), declines
clean, 0 lost; write-backs nonzero with a target locked.

## Phase 3 built, 2026-09-28 (e4d1df1d)

The hologram pass's eleven families join the crisp take as one family
(kHoloGeneric, matched by VS hash through holo_families.h -- the shared
list the take, the depth pass and the census now read; moved, not copied):
the radar contacts, the ship and target holograms, the icons, and the
world-marker reticle. The canopy is deliberately not on the list (it sits
in front of the whole sky; covering it would smear the stars behind it --
the depth pass's own reasoning). Sean opted in: the pitch-swim on those
elements reproduces on main, so it is the AA pass's reconstruction, and
taking them out of it is the fix.

Their states were never census-measured (Phase 0 watched the three named
families), so the refusal net is the safety and the census's new fourth
family watch ("hologram": ga state lines, window counts, G-C votes, G-D
pairs) doubles as their G-A measurement in the Phase 3 flight. Additive
converts exactly and covers nothing (transmittance untouched), so the
take adds no coverage of its own; what differs from stock is WHERE the
light lands. THE open question, judged visually: for additive elements
stock computes T(F+L) and the layer gives T(F)+T(L) -- over a bright
background a glow reads differently, and no instrument answers it (G-F
measured premultiplied-over; the parity capture is keyed to the holo
pair).

The hologram pass's re-issues skip taken holograms through the
family-blind !layered gate; its 30 s line keeps counting listed draws
while declining eye-frames "nothing listed" -- expected (a reading note
sits at both). Rig: 295 checks, 0 failures; hologram_depth_test 2309
PASS. Installed to Steam as v0.18.0-rc.3-31-ge4d1df1d.

Phase 3 flight: HMD 0.5/0.75 with DLSS, ui_quality = 100. Radar contacts,
ship and target holograms, icons sharp and steady through rapid head
pitch; the world-marker reticle on a distant target; a station approach
for the additive-glow-over-bright question (the halo's read on the
holograms); the canopy unchanged. The log: a "hologram" row in the 30 s
table with taken counts, no refusal naming a hologram, declines clean,
and the census's ga hologram state lines as the G-A record. Then the
revisit Sean named: with everything rendered through one path, re-read
the remaining differences (the lost halo, the translucent regime).

## The phase-3 review round, 2026-09-28 (c4253e6d)

reviews/crisp-hud-phase3-review-2026-09-28.md found two defects and
nailed the missing-mesh question's shape. Both fixed, and the mesh loss
localized:

- R1 (P1): the target sphere's PSes integer-Load scene depth at
  SV_Position pixel coordinates; the layer's larger viewport breaks the
  addressing (WARP-reproduced). R2 (P2): the corona family paints the
  radar glow AND the real sun; a hash cannot distinguish the uses and the
  take has no radius concept. The take now admits only the eight
  radar/icon hologram families (kHoloFamiliesTake); the sphere, the
  corona family and the world-marker reticle refuse to stock with the
  review's citations. The depth pass keeps all eleven and its radius
  clip, unchanged; the two lists are separate in holo_families.h.
- The ship/target MESH holograms (kHolo, Phase 1): the review's offline
  replay of the captured draws (build/review-mesh/REPORT.md, from
  pool/panels_102548.bin) was CLEAN at every stage -- rasterize, depth,
  overwrite, the real captured tonemap draw, composite. The session
  evidence instead: the game's tonemap ordering varies frame to frame,
  and content taken into the HDR layer AFTER the eye's re-issue can never
  publish -- the layer clears next frame. The counters counted the draw
  and the publication while the pixels were discarded. Fixed with a
  refusal, not a counter: a crisp draw after the eye's tonemap re-issue
  refuses to stock (kToneLate), so the meshes render exactly as the game
  drew them in those frames; crisp post-tonemap content would need a
  second re-issue (new machinery, out of scope).

Rig: 297 checks, 0 failures (the pre-fix rig fails exactly the three new
refusals -- discriminating evidence). Installed to Steam as
v0.18.0-rc.3-33-gc4253e6d. Phase 3 re-flight: cockpit with the ship and
target holos up -- present in every frame now (stock in the frames the
game tonemaps early, crisp where ordering allows; the 30 s "left" line
names the kToneLate frames), radar icons/contacts crisp, menus over
panels, the sun never taken (R2's guard).

## The timer fix + the holos' stock state, 2026-09-28 (53fd633f)

The phase-3 flight's follow-ups: Sean saw the ship/target holos in the
mirror (stock, not taken) and a large perf hit in open space (87 -> 72 fps
in the expensive window, ~3.3 ms unaccounted inside "door"). Both read:

- The holos are the R1-refused sphere family by design (their per-pixel
  scene-depth reads break under the layer's viewport scale; taking them
  lost 84% WARP-reproduced). They render stock everywhere -- visible in
  mirror and headset, not crisp. kToneLate fired zero times: nothing is
  silently lost anymore. Crisp wireframe holos need the layer-sized depth
  input -- the real R1 repair, the next feature chunk.
- The perf hit is real and was unmeasurable: the route-timer ring (64
  slots) held one frame of intervals while two frames' worth were
  unresolved under load -- 60,895 "no free timer", tonemap/coverage
  timings absent (the phase-3 review's flag). Fixed: kRouteRing 512, and
  routeSample reservoir-sampled so the 30 s percentiles are unbiased at
  any interval rate. The next flight's price lines attribute the cost;
  the moved HUD shading at 5000x4835 (the largest term) is untimed by
  design -- the fps delta is its honest measure, and ui_quality 100 vs
  125 is the lever.

Installed to Steam as v0.18.0-rc.3-35-g53fd633f.

## 2026-09-28: actual model surfaces and a narrower repair

The saved `review-mesh` images contain station/health/shield rings with empty
model interiors. Its `81216/A296` t2 is a HUD atlas. Those tests do not prove
that the actual ship/target models entered the crisp path.

Verified Frontier `be70af2a` census `174242`, 17:46:05–06: two `5559` quads
use PS `EA02FAC2BD6C643C`/`E95634B0F61D218F`. t1 is original 2016×1948 R32
linear depth. t4 is 512×512 depth-like data; shader gradients reconstruct
edge light, with matching-size t5/t6 on one variant. Four offscreen 512²
R32 mesh draws (`5B0068AF5630F96B`/`6C416587F7C22B97`, 62688/7584 indices)
are candidate producers; exact consumer/producer identity remains unproven.

First test an inverse viewport/jitter map at each exact PS's single
SV_Position→integer t1 load, preserving all UV/material reads and original
linear-depth encoding. Then take complete composites after DLSS with original
alpha, occlusion and stencil04 write-back. This targets history smearing
without another full-eye depth pass. A copied layer-sized R32 input instead
costs a pass and ~98.2 MB/eye at UI 5040×4870. Neither adds detail to t4.

512→1024/2048 model sources mean 4×/16× pixels plus matching t5/t6 and edge
width changes; identify producers and price these separately. Latest
`kToneLate=0` does not justify a second tonemap. Corona remains excluded.
No production admission change yet. The ignored
`build/sphere-remap-prototype/RESULT.txt` records 4,907 WARP checks with the
two real pixel shaders. Both reproduce enlarged-grid loss and recover all
25,600 controlled pixels. All 64 nonuniform-depth cases preserve stock RGBA
byte for byte, including translucent alpha, at .5/.75 input scale, UI100/125,
both jitter signs and subrect/scissor. One MAD repairs only the t1 address;
512² model data stays unchanged. Actual VS geometry, classifier admission,
private stencil/write-back, device lifetime and headset performance remain
unqualified. Fresh post-DLSS composition is the recommendation for history
smearing; source-image blur is a separate question.
## 2026-09-28: validated target-hologram admission and final capture

Production remaps only VS5559 with verified EA02/E956 pixel programs. Their
grey target sphere identity was pixel-probed in Frontier 20260924_155636,
frame8548/eye0/point3 (hologram-depth arc); red wireframe81216/A296 is a
different, already admitted family. Exclusive cockpit use, current model
placement and actual VS geometry remain live qualifications.
Unknown/linkage/identity/depth/viewport failures keep the complete stock draw.

The bounded cache holds two patched programs and one float4 CB; original
t1/materials/alpha and512² sources remain intact. The free shader b13 is
saved/restored independently of the original PS/classes. Preparation precedes
admission; successful Begin counters prove routing setup, not GPU execution.
Stock colourless stencil/depth replay uses restored original bindings. One
bounded restoration retry handles transient setter faults; persistent failure
retains originals and blocks unsafe owner direct/DrawAuto/indirect/replay
issues until shutdown (since 2026-09-29 only to the next frame boundary; see
the last section). No extra full-eye depth copy or runtime HLSL.

Actual-PS fixtures pass927 checks each on WARP and RTX5090, including64 exact
nonuniform RGBA cases, original DSV byte equality,
source/device/cache/allocation failures and before-real-setter faults.
Classifier4157/0 and FinalCrisp624/0 pass. Full SDK/profile validation,86
pooled rigs, Python tools and installer resource checks pass in
`build/dlss-holo-final-full.log`; receipt input
`665ab62402e1ff2363f8b10b040fa11d7b9d5ef11f7f6aee6830f3f007a7feb1`. Promote
this same source to Frontier; main stays separate. Headset quality/performance
remains unqualified.

FinalCrisp adds both-eye native1400² crops and first-pair full overviews after
`uiLayerComposite`, preserving P/T/L0's earlier stages. AA-independent boundary
epochs plus native stereo sequence prevent guessed joins and retain the
sixteenth right eye after old flush. JSON records original-stage sizes/mapping,
native ROI/region, flips, composition/passthrough and
copied/written/failed/missing outcomes. Fault-published staging alone is never
success. Current4032×3896 RGBA8 run costs359.1MiB under512MiB total/64MiB
per-artifact caps; other formats can explicitly decline. No capture COM/GPU
work outside an armed dump.

Desired scanner <> centers are(.5861,.5916)/(.5939,.5919); previous probes
missed both and cannot identify redirected private-HDR contributions.
Shared94D5 glare remains unadmitted without spatial proof. The next single dump
must compare FinalCrisp with matched old stages and qualify target-holo
transparency/occlusion plus ship blur. Saved red hull has82–86% joined engine
motion; this rejects wholesale missing motion, not wrong vectors. No sharpening
compensation or source-resolution inflation.

## 2026-09-29: final capture confirms cockpit hologram admission

Frontier `7aaaf39c` gfx `033435`/runtime `033436_691_11608` verifies. Sean
reports both holograms look great. Dump `033645` writes all 34 FinalCrisp
images, matches 16 stereo pairs (scene 12224–12239/native 10555–10570), no
missing/unmatched/duplicate images. Both target and own-ship models are absent
from matched P/T/L0 and visible/detailed in final composition. First right
crop/overview report passthrough; later pairs report composition. This is
qualitative model validation, not a controlled alpha/occlusion test.

The scanner yellow <> remains visible before final composition. Added contacts
overlap its region, so final changed pixels do not identify its shader; current
probe positions miss it and saved E508 changes no pixels there. World brackets
and 30–34% of bright pixels in a compact distant-ship ROI change with final
composition while sharpening is OFF. That establishes overlay contribution, not
actual ship mesh admission. No broader shader admission follows from it.

CPU spikes worsen approaching ships. The earliest 140–231 ms post-submit
cluster precedes the first model remap, ruling out this new route as its cause.
DLSS stage time remains stable, HUD seed workload increases, and resource
bursts correlate with stalls. Exact cause remains open: see the performance
arc. Next 90 s CPU/GPU trace uses the existing installed build/PDBs,
diagnostics OFF, no eye dump. Exclude 03:36:45.300–49.772 capture/readback from
this flight's performance comparison.

## 2026-09-29: mistaken selector admission withdrawn

Sean requested unwinding the <> selector work so another agent can address it.
The attempted admission added in `1ff8c224` was the world-space bracket pair VS
`71DD8B8B09060A81` / PS `2D037A047171BF3B`, not the scanner-rim glyph. That
distinction was already established by his pictures; no new shader admission
follows from this rollback.

The unwind removes that exact pair from the crisp family rule, its added PS
constant/observation, dedicated `ui_reticle_test.h` fixture and unused
fixture-only stencil-reference arguments. A classifier check now requires the
known pair to remain outside the crisp layer. Preserve its pre-existing
world-marker depth/motion and flat recipe, the eight safe cockpit families, the
EA02/E956 target-sphere remap, every precompiled shader, and general final eye
capture. No selector-specific eye-dump probes were added in this arc.

Focused UI rig passes 2,904 checks, zero failures, and its dry-run creates no
device/files (`build/diagnostic-focused/selector-unwind-focused.log`). Changed
graphics code compiles, including `vscreen.cpp`. Independent source/fixture
review found no scope or regression issue. Full validation passed all 86 jobs
in 125.9 s, native 4,904/0, UI quality 2,907/0, Python self-tests, production
DLLs and actual installer resources (`build/dlss-selector-unwind-full.log`).
Receipt input hash
`66d05e4444e0f1e7f767c6b6d6d2f69f10d3a4a96aee36ea27f55034ce2cd210`. Clean
source `967f0519` is pushed on the separate branch. Receipt-guarded DLL
promotion passed, and Frontier dry-run/install/payload/native-receipt
verification passed as `v0.18.0-rc.3-68-g967f0519`. Receipt
`edvr_native_receipt.json.pre-967f0519-20260929-061148.bak`; full transcript
and unchanged INI/DLSS hashes are in
`build/dlss-selector-unwind-frontier-install.log`. Main and Steam unchanged.
Scanner identification stays deferred to the other agent.

## 2026-09-29: the restore fence lifts at the frame boundary

Review B1. A hologram PS/b13 restore that fails twice (both guarded
attempts) raises `g_uiLayerIssueBlocked`, and the VR draw thunks then dropped
every game draw on the owner context until device shutdown: a black or frozen
headset until a restart, for one rare driver fault. The Phase 3 entry above
says "until shutdown"; that is now "to the next frame boundary".

`Binding` records EDVR's own patched PS and constants pointers at `begin()`
(identity only). `Binding::settle` puts the saved PS and b13 back only into a
slot that still holds those; a slot the game has rebound holds the game's own
state already and only has its saved reference released. Each getter and
setter is guarded, and the b13 slot is settled even after a PS fault.
`uiLayerFrameBoundary` runs it first (`settleAtBoundary`): success clears the
binding, lifts the fence and logs once; the eighth failed boundary in a row
lifts the fence anyway with a loud line and keeps the saved references (the
hologram take then stays refused). The layer itself stays stood down for the
session either way; toggling `fix.ui_quality` off and on re-arms it as before.

Built, not flown. ui_holo_test went from 927 to 1059 checks on WARP: rebound
slots left alone, persistent setter and one-shot getter faults, reference
counts as the leak meter, the seven holds and the eighth fail-open. The glue
in `ui_layer.cpp` only logs and flips the flag, and no rig compiles that file.
The trigger is a rare driver fault, so a flight is unlikely to exercise it.
The log lines to look for are "restored at the frame boundary" and "could
NOT be put back".
