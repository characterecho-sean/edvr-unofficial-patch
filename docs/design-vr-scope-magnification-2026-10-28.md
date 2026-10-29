# VR scope magnification: the game's zoom inside the scope (design, 2026-10-28)

Parent arc: `docs\design-native-onfoot-vr-2026-10-13.md` (F141 to F153). Native on-foot VR puts the ADS eye on the game's sight line
E -> S at one depth per weapon (F150, F152). A scope does not zoom in VR, because a headset cannot change its FOV. This design shows the
game's own zoom as a magnified picture inside the scope's lens. The rest of the view stays at true scale.

## Status

- **State: S0 FLAT TWICE, VR TWICE** (`d90db147`, measure only, no key; section 7). Settled: Q1 for every owned weapon, Q2 (VR has no
  zoom field: flat table), Q3 (the ADS FOV is absolute per weapon), Q4 (see-through glass, the footprint = the hole in the frame's
  depth), Q6 (a chevron and a tint on the glass, in the scene colour, no UI over it). No rendering code. Branch
  `claude/vr-scope-zoom-design-6ef950`.
- ruled out: every S0 lens census so far (the probe watched the left eye at the right eye's points).
- **Flat zoom MEASURED** (section 7, Epic 2026-10-28 and 10-29): the world's camera (kind 0, FOV at +0x280) settles at an absolute FOV
  per weapon, the same at `HumanoidFOV` 56.327 and 64.752 deg: the scoped Executioner 15.000 deg, the bare one 35.000, every other weapon
  48 to 52. M therefore depends on a reference FOV that EDVR must choose: scoped 4.07 or 4.82, bare 1.70 or 2.01, the rest 1.10 to 1.42
  (from this rig's two settings). Kind 3 is the weapon's own camera (47 deg at the hip, not the option).
- **A against B3 at M 4 to 5** (section 2, 2026-10-29): A gives the scoped lens 21 to 25% of its density, B3 full density at high risk.
  Every other sight is at M 1.10 to 2.01, where A is enough, so A is built either way; B3 only if A's scope is too soft.
- **Recommendation: option A** (section 2): after DLSS and RCAS, copy the aiming eye's picture inside the lens, magnified by the game's
  zoom M about the sight point (the shield ripple's route). Under 0.1 ms, one eye, scoped ADS only; no scene pass, no motion vectors, no
  DLSS change; F106 carries it to the left eye. The price is detail: 1/M of the output's density.
- **Phase 2, only if A is too soft:** B3, the left eye rendered as the zoomed view (a pass F106 already covers). Risks in section 2.
- **The zoom's source is not found yet** (static RE, build 334925, section 1):
  - ruled out: the bend's per-frame pair (weapon data +0x74/+0x78 to `[act+0xB0]->vt+0x18`) as the zoom, because its receiver is
    HumanoidAimAssistComponent (setter 0x1A8C350): aim-assist tuning, not the "zoom/FOV floats" the analysis note named.
  - ruled out: EDVR's F8 census FOV (`RC+0x16C`) as a VR zoom field, because it read -1.000 through every ADS press of
    `edvr_gfx_20261028_060723`, the scoped plasma sniper included.
  - ruled out (S0 VR, section 7): RC+0x16C/+0x17C and the refresh hook's cameras as VR zoom fields (-1, and nothing narrowed).
    Unread: the stat `ScopeMagnification`. M in VR comes from the flat table.
- **Questions** (each has a log line in the first flight, section 6):
  Q1 the zoom per (item, scope); Q2 which field carries it per frame (flat, VR); Q3 absolute FOV or relative to the player's FOV option;
  Q4 what is drawn in the lens (see-through glass, opaque screen, reticle); Q5 the lens's footprint and the sight point's offset in it;
  Q6 what must stay unmagnified over it (readout, AR gunsight, HUD). All answered for the Executioner; for the unscoped sights only Q1
  to Q3 (no eye dump has seen their sights).
- **ruled out (cost or brief):** a third camera pass (C), magnifying in DLSS's input (E), a whole-view FOV change (D). Section 2.
- **Analysis hand-over FILED** (analysis `1547986`): `analysis\decomp\verify_20261028_ads_zoom_receiver.txt` (the receiver, the correction to
  `analysis\decomp\verify_20261027_ads_sight_line.txt` section 1, the candidates).
- **A1 FLOWN 2026-10-29** (`5a47ad8f`, Epic VR; section 8): the lens ran on the bare and scoped Executioner, the Oppressor and the
  Aphelion; depth, eye numbering and cost (0.07 ms) as planned. The pilot: "a really cool effect where it makes sense"; the chevron is
  magnified with the world (Q6's prediction), and only optical sights should zoom.
- **A2 FLOWN 2026-10-29** (`ce3f9bac`, main merged at `40d7409b`; section 9): optical sights only; the AR gunsight quad (VS `025B4B9F...` /
  PS `46F92DC7...`, 6 indices, the readout atlas) is the layer family `kScopeMark`, drawn after the lens, one a frame an eye, depth seeded.
  The pilot: "the chevron looks much clearer now"; it is brighter than stock (tonemapped alone, T(F) + T(M) against stock's T(F + M)).
  The lens declines for the left aiming eye (`fix.native_on_foot_ads_sight_eye = left`; the left eye is a later build). c lags the picture
  by one frame on every frame (measured, not changed).
- **A3 BUILT 2026-10-29, INSTALLED ON EPIC (both), NOT FLOWN** (`c738e367`, main merged at `dcfd0608`; section 10, "As built (A3)"): the mark is drawn into the eye's additive weapons layer and composited after
  the lens by the tonemap difference (stock brightness); the weapons half governs it, the scope zoom key arms it; the S0 census probes optical
  sights only; the press line counts `composites matched/plain/none` and `--health` makes a taken mark nothing composited a PARTIAL. Its review
  fixes (same section) make the mark's stand-down last a press, not the session, and count it in the press line.
- **Next, B3 for the scoped sniper only** (the maintainer, 2026-10-29, after A2's flight: the 4x lens is "a little blurry"). A keeps 1/M of
  the density, so at M 4.06 the lens shows about 90 x 50 output pixels (45 x 25 rendered) over the glass. B3 renders the unseen left eye
  (F106's both mode) as the zoomed view. Designed after A3 has flown.
- Keys: `fix.native_on_foot_ads_scope_zoom` (off until flown OK) and the temporary `experimental.native_on_foot_ads_scope_zoom_fov` (the
  reference FOV, 56.3 by default).
- Separate arc: a flat stutter on the scoped Executioner's ADS (2026-10-29), in its own doc,
  `docs\investigation-flat-scoped-ads-stutter-2026-10-29.md`, on its own branch.
- **Key (after S0, on the maintainer's word):** `fix.native_on_foot_ads_scope_zoom` = on | off. Section 5.

## 1. Where the game keeps the zoom

What is known:
- Flat scoped ADS moves the camera back and narrows the FOV. For the plasma sniper, the game's E sits at +5.00 cm from P14 scoped and
  +16.00 cm unscoped (F149, the same to two decimals in every flight).
- A community chart (flat screenshots at 150 m) shows that most scopes keep the camera and the target's size, and that the Executioner
  (`$wpn_m_sniper_plasma_charged_name`) zooms about 1.8x. This is a guide only, not the game's number.
- Flat ADS keeps the eye on its locator and bends the shot through S (`analysis\decomp\verify_20261027_ads_sight_line.txt`). So the zoom is
  an FOV change only, with no camera snap.

Static RE this session (`analysis\decomp\verify_20261028_ads_zoom_receiver.txt`, analysis `1547986`; READ unless marked):
- The bend's pair is aim assist. 0x1908760 sends floats +0x74 and +0x78 of `[[act+0x140]->vt+0x20()]+0x2F0` to typeId 0x5F77034 on every
  call. Constructor 0x19CE660 provides that interface at comp+0x58, and its slot +0x18 (0x1A8C350) stores the two floats at +0x148/+0x14C.
  The registrar names the component `HumanoidAimAssistComponent` (size 0x1E0, pool 0x5F6D320). HumanoidEyeComponent also depends on the
  same interface (INFERRED: the aim-assist blend at eye vt+0x90).
- Candidates (NOT FOUND):
  - Z1, the camera settings FOVs the readout reads: `[AR+0xE0]->vt+0xE8()` +0x16C and +0x17C.
  - Z2, a camera FOV lerp with a start, a target and a rate, debug printers at 0x106D480..0x106D8A9. "FOVRate" is looked up by name in a
    parameter set, and the FOV modes include `FOVFromGenericParamCamera`.
  - Z3, the weapon stat `$HumanoidHUD_Stats_Weapon_ScopeMagnification;` (registered at 0x1989585): the game holds a magnification value
    somewhere, perhaps per scope.
- MEASURED: the F8 census's `RC+0x16C` read 1.571 rad and then -1.000 from 06:12:31.9 to the end of `edvr_gfx_20261028_060723`. The first
  ADS press was at 06:12:39, and the census prints any change. So in native stereo that field carries no zoom.

How EDVR reads it, by what S0 finds:
- **If a field carries the zoom in VR** (Z1 or Z2 narrowing with the ADS fraction w): read it in the hooks that already run each frame,
  F141's bend hook or the F85b readout getter, which already has AR. This costs nothing extra.
- **If VR computes no zoom** (likely: the -1 above suggests the VR camera path skips the flat FOV): a table per (item, scope), measured
  once in flat from the game's projection rows, as F149 baked the camera depths. The held copy's scope comes from `heldScopeOf` (F151).
- M is defined as tan(f_hip / 2) / tan(f_ads / 2), the factor by which the game enlarges the target in flat. If the game's scoped FOV is
  absolute, not a fraction of the player's FOV option, M depends on that option (Q3). The table then stores both FOVs, and M uses the
  default option.

## 2. Rendering options

Environment: Pimax Crystal Super on Pimax OpenXR (also Quest 3 over VD). Render 2016x2017 per eye, DLSS preset K, output about 4032 square
per eye (F70, F105), 90 Hz, so 11.1 ms a frame. F106 ("ADS: sight in both eyes") is on by default. The right eye is the aiming eye: the
sight lock solves it onto the line.

| Option | Extra GPU / frame | Extra CPU | Lens detail | DLSS / MV | Risk |
|---|---|---|---|---|---|
| A crop and magnify after DLSS | about 0.02-0.1 ms, one eye, scoped ADS only | one re-issued draw | output density / M | none | low |
| B3 left eye renders the zoom | none (reuses a covered pass) | none | full render over the lens | left-eye history reset at each edge | high |
| C third camera pass | about 1.5-3 ms (estimate) | one more view's submission | full | own DLSS instance | not feasible |
| D whole-view FOV change | none | none | full | reset | rejected by the brief: the world must stay at true scale |
| E magnify in DLSS's input | as A | as A | input density / M | MVs wrong in the lens | rejected |

**A: crop and magnify after DLSS (recommended).**
- The scope's lens draw is re-issued into a per-eye coverage layer (section 3). At the door, after RCAS and before the weapons/HUD composite
  and the UI layer (the shield ripple's slot), the lens's bounding box is copied and rewritten over the coverage:
  out(p) = copy(c + (p - c) / M). c is the sight point's projection, and the offsets are in the eye's tangent plane, which is exactly the
  flat zoom's scaling. Exactness off-axis on canted views: rotate to c's direction, scale the tangents, rotate back (F106's rotation maths).
  Bicubic sampling; a 1-2 px feathered edge.
- Cost, estimated: a lens 15-25 deg across is about 0.1-0.4 MP of the 16 MP output. A copy of the box, 16 taps a pixel and one mask draw come
  to well under 0.1 ms on the pilot's GPU. Zero when not scoped. To be measured by GPU timer in the first build, not assumed.
- Detail: the lens shows the DLSS output at 1/M of its density (at M = 1.8, about 55% of the unmagnified pixels per degree). The target looks
  1.8x larger with no new information: a digital zoom. If the paused foveated DLSS work lands, its sharper centre feeds A for free (the fovea
  composite also runs after DLSS; A goes after it).
- DLSS and motion vectors: untouched. The lens is made after the upscale, so the history never sees it, and DLSS's residual shimmer is
  magnified by M.
- Stereo: the right eye only. F106's share copies the right eye's finished picture (lens included) into the left eye. With F106 off, the
  left eye keeps an unmagnified view of the scope body, as a real scope looks with both eyes open.
- The runtime's reprojection moves the whole picture by the head's turn, but the lens's content should move M times as much. On a
  reprojected frame the lens is therefore off by (M - 1) x the head's turn since render: small at 90 Hz and M under 2.

**B3: the left eye as the zoomed view (phase 2, only if A is judged soft).** With F106 on, the left eye's render is covered in ADS by the
right eye's image. During scoped ADS, B3 gives the left eye a narrow frustum along the sight line (its axis parallel to the right eye's line;
6.3 cm of offset is 0.04 deg at 100 m). The narrow tangents would come from EDVR's runtime and the turned pose from the eye-producer hook;
whether the game re-reads an eye's projection every frame is not known. Its picture is then composited into the right eye's lens before the
share. The lens gets full render detail and no pass is added.
- The left eye's DLSS history needs a reset at each scoped ADS edge.
- The FP weapon must be skipped in that eye (stencil 0x10 draws), or the scope fills the narrow view.
- The left eye's own periphery outside the right eye's coverage (F106's 3 deg band) must go dark.
- It needs F106 on (with F106 off, fall back to A).
- F113's eye rows, the readout pin's both-eyes pairing (F148, F151), F110's model eye and the marker fix assume two like eyes; each would
  have to skip a zoomed one, and the light-grid cache would need checking.
- Not before A has flown and the pilot calls it too soft.

**A against B3 at the measured zoom** (2026-10-29; S0's numbers, section 7; the Pimax rig above: about 33 output px and 16.5 render px a
degree near the axis). The scoped Executioner's opening is about 11 x 6 deg, some 360 x 200 output px. At M 4.07 (from 56.327 deg) it shows
2.7 x 1.5 deg of the world; at M 4.82 (from 64.752 deg), 2.3 x 1.2 deg.

| At the scoped Executioner | A (copy after DLSS) | B3 (the left eye renders the zoom) |
|---|---|---|
| Detail in the lens | 89 x 49 output px (M 4.07) stretched over 360 x 200: the world at the unzoomed 33 px a degree, made 4x larger; 25% of the lens's own density (21% at 4.82); DLSS shimmer magnified 4x | the left eye's tangents divided by M (93.6 deg to 29 deg tall at 4.07): 134 output px a world degree, the lens's full density, 16 times A's pixels; 99.5% of that render is thrown away |
| The game's chevron | magnified with the world: 3.7 deg below c at 4.07, inside the lower edge; 4.3 deg at 4.82, past it | gone: it is on the glass, part of the weapon the zoomed eye must skip |
| Extra GPU / CPU | a 72k px copy, well under 0.1 ms (estimate) | no pass added; the narrow view may pick finer LOD at distance, as flat's 15 deg view does (the flat scoped-ADS stutter investigation bears on this) |
| Unknowns | none new | whether the game's culling and LOD follow a narrowed eye; whether its motion vectors carry the zoom's ramp, or the left eye's history resets at each edge |
| Touches | the shield ripple's slot | F106 (required), F113, F110, F148/F151, the marker fix and the light-grid cache, each assuming two like eyes |

- B3's first unknown is answered by EDVR's own code (INFERRED): the game rebuilds each eye's projection every frame, because EDVR's
  DLSS jitter reaches it that way (a per-eye tangent shift from `src\d3d11\native_temporal.cpp`, applied in `GetProjectionMatrix` in
  `src\openxr\openvr_system.cpp`). Narrow tangents could ride the same route. Whether culling and LOD follow a narrowed eye is untested.
- Shared, not a difference: on a reprojected frame the lens's content is off by (M - 1) x the head's turn since render. That is 3 times
  the runtime's residual at M 4.07; on a late frame at 100 deg/s, about 3.4 deg across an 11 deg lens. B3 is composited after the render
  and reprojected the same way. A mark EDVR draws at c (the impact point at every range, section 3) would serve either option and does not
  depend on the chevron's draw.
- **Recommendation: build A first, for every sight.** Every unscoped sight and the bare Executioner sit at M 1.10 to 2.01. There A keeps
  50 to 90% of the detail, and B3 would carry all of its risk for little gain, so A is needed whatever the scope gets. Fly A on the scoped
  Executioner. If it is too soft, which 25% density makes likely, B3 becomes phase 2 for M above about 2.5 only. Its first instrument
  narrows the left eye through the jitter route and logs the frame time, the cull and the LOD. A lower reference FOV (section 4) keeps
  M nearer 4 and the chevron inside the lens.

**C: a third camera.** The game's native stereo renders exactly two views, and EDVR cannot add one without replaying the scene.
Draw-bound passes cost about a full eye even at a small viewport, and settlements are already CPU-bound (the settlement CPU arc). Rejected on cost and feasibility.

**E: magnify inside DLSS's input.** The lens content would move M times as fast as the game's motion vectors say, which ghosts unless the
lens's vectors are rewritten, and it starts from the 2016 input. Rejected.

## 3. The lens on screen, per frame

- **The centre c.** S, the sight locator (`Root!Ui_AR_Rifle_ADS_Gunsight` / `..._Pistol_...`, or the Optics attachment's own), comes from
  F144's settled line rebuilt on this frame's P14 and root (`adsSightTick`). The eye sits on the line E -> S, so every point on that line
  projects to c. Shots go along it, so c is the impact point at every range.
- **The footprint, first choice:** the scope's lens draw, named by the S0 census (Q4). It is re-issued into an R8 coverage layer through the
  route F105 (world screens) and the shield ripple already use, and keyed per eye by mesh, ordinal and instance count as F151 binds a lost
  readout draw.
- **The footprint, fallback:** the hole in the weapon's stencil (0x10, F119) around c, bounded at a maximum angular radius.
- **The sample disc** is radius r / M about c and must stay inside the aperture. That holds while |c - lens centre| < r (1 - 1/M), 0.44 r
  at M = 1.8. Outside that, clamp M down for the frame and count it.
- **Drawn over the lens, unmagnified:** the HUD and the weapon readout come after the door through the UI layer. Whether the readout's
  space-1 parts and the AR gunsight part (anchored at the same gunsight locator) land before or after the door is Q6. A part that lands in the
  scene under the lens would be magnified with it and must move after the door.
- **If the lens is an opaque screen** (Q4; dump 193900687001 shows the Executioner's box sight with a tinted rear face at the hip), the world
  is not drawn behind it. Take that draw out of the scene pass, so the world reaches DLSS through the aperture, and re-issue it over the
  magnified picture, or go to B3.

## 4. Which weapons

From the game only, per (item, scope), filled by S0's flat flight. The pilot's loadouts hold ten weapons (F149's camera-depth table).
Scoped copies flown so far: the plasma sniper only. The chart expects the Executioner scoped at about 1.8 and the other scopes near 1.0.
An entry under 1.05 gets no lens: magnification that small would only cost detail. Unscoped ADS is measured too (Q1), in case the game
narrows the FOV slightly without a scope.

The maintainer owns no other scoped weapon (2026-10-28), so the scoped table holds the Executioner alone (15.000 deg; the chart's 1.8
does not match it). Any other scoped (item, scope) gets no lens until a value is measured (S0's flat line, flown by someone who owns
it) or the game's own value is found (Z3, the `ScopeMagnification` stat, by static RE).

The game stores each ADS FOV as an absolute angle (Q3, section 7, 2026-10-29), so the table holds FOVs, and M = tan(h/2) / tan(ads/2)
against a reference h that EDVR must choose: VR has no hip option to take it from. This rig's 56.327 deg gives the scoped Executioner 4.07,
the bare one 1.70 and the unscoped sights 1.10 to 1.20; its 64.752 deg gives 4.82, 2.01 and 1.30 to 1.42. Every unscoped weapon narrows
(48 to 52 deg), so the 1.05 line excludes none of them. Whether the feature is for scopes only or for every sight is the maintainer's
call, and an unscoped sight's lens would first need its own Q4: only the Executioner's glass has been seen.

## 5. Config surface

- `fix.native_on_foot_ads_scope_zoom` = on | off, live, menu "ADS: scope zoom" under "Aiming down sights". It names what the player gets.
  It is a user key, not a test key: magnified motion under "ADS: aim with head" is a comfort choice. Default on once flown OK.
- `experimental.native_on_foot_ads_scope_zoom_fov` = the reference FOV in degrees, 56.3 by default, live, a menu row "ADS: scope zoom
  FOV (experimental)" (the maintainer's word, 2026-10-29). M = tan(h/2) / tan(ads/2) from it. Temporary: when a value settles it becomes the
  behaviour, and removing the key is proposed (AGENTS.md).
- There is no other strength control: the ADS FOV is the game's. S0 itself is measure only, with no key, as F141 was.

## 6. The first flight: S0 (measure only, no rendering code)

**As built** (`84d13bfa`): pure logic in `src\d3d11\scope_zoom_s0.h` (rig `tools\native_frame_test\s0_checks.h`), glue in
native_on_foot.cpp. Four lines, each a `--health` rule under the tag S0 (NEVER APPLIED when presses ran and the line has nothing):
1. `scope zoom (S0) flat` (Q1, Q2 flat, Q3), one per ADS press: Status.json's ADS bit with on foot (re-read every 100 ms; the game
   writes it about once a second, so a 3 s lag guard). It logs the held item, and for every camera that the view-constant refresh hook noted
   (FUN_1405922f0; camera +0x280 vertical FOV, +0x260 aspect; in flat while a temporal mode is on) its FOV at the hip and settled in ADS,
   with M and the ramp. Flat has no scope state (the loadout reader is VR only): the flight card's order names the copy. **Reading:** the
   camera that narrows gives M; an ADS FOV that stays put when the FOV option moves the hip means absolute FOVs (Q3).
2. `scope zoom (S0) vr` (Q2 VR), per press: w; RC+0x16C and +0x17C, the F8 census's settings object, which the RE note says the readout
   reaches as `[AR+0xE0]->vt+0xE8()` (read by the census's pointer walk, no game call); each eye's p11 from its scene rows (a control that
   must not move); the refresh hook's cameras (only while the VR camera census or the world route runs that hook). **Reading:** a value
   that narrows in scoped ADS is the per-frame field; none means the flat table.
3. `scope lens geometry (S0)` (Q5), at the first settled frame (w = 1 for 0.6 s): c (F141's S through each eye's rows) in NDC, pixels and
   degrees off axis; the game's |E - S| against EDVR's right eye to S.
4. `scope lens census (S0)` (Q4, Q6), at that moment for the first three scoped presses and the first unscoped one: the pixel probe's four
   16x16 windows in the right eye at c and at 1.5, 4 and 8 deg from it. Its own lines follow, one per draw that changes a window (VS/PS,
   blend, depth, count, texels changed). The probe keeps those points, so an eye dump in scoped ADS probes the same places. **Reading:** a
   blended draw over world depth is glass (A applies); an opaque draw nearer than S is a screen; a small draw at c only is the reticle. Q6:
   a readout or gunsight draw among them is in the scene under the lens and must move after the door; the dump's treated against final
   crops show what the UI layer adds after the door.

Changed from the plan:
- The planned lens census and overlap lines became one census on the existing pixel probe (four points, not a ring), read with the eye dump.
  A new per-draw coverage pass is more instrument than the question needs.
- Z1 is read through the census's walk.
- Flat's FOV comes from the camera the projection is built from. Nothing decodes P00/P11 in flat.

Unconfirmed: whether the probe sees native-stereo eye draws. The census rule checks its `eyes seen`.

**Flight card.**
- Flat (Epic, `--scope both --xess` as that install is, the game in flat mode): every weapon, hip to ADS three times, each held 3 s or
  more; scoped copy then unscoped (the Executioner both, and any other scoped weapon owned). Change the on-foot FOV option
  (`HumanoidFOV`, not the ship's FOV) once and repeat the Executioner.
- VR (Epic's both install with the game in VR mode, `advanced.vr_camera_census` on for the camera source; set in Epic's edvr.ini
  2026-10-28): the scoped Executioner in ADS, 10 s still and 10 s turning the head; the unscoped copy; one eye dump in scoped ADS. The
  first three scoped presses arm the probe on their own.
- Read `edvr_log.py --expect-build <installed build> --health` first, then the four lines.

**After S0:** see-through glass and M under about 2.5 for every zooming weapon: build A behind the key. An opaque screen: A with that draw
moved after the door, or B3. Zoom above 2.5: A's detail is likely too low, so put B3 to the maintainer. A per-frame VR field found: read it;
otherwise bake the flat table.

## 7. Journal

**2026-10-28, S0 flat flight** (Epic, `edvr_gfx_20261028_153658`, `d90db147`, build matched). `--health`: S0 flat OK, 4 presses, all
the Executioner (the loadout: PrimaryWeapon1 class 5 with `weapon_scope`, PrimaryWeapon2 class 2 with none). By the card's order, presses 1
and 4 are the scoped copy and 2 and 3 the bare one.

| Copy | kind 0 (16:9) and kind 4 (1:1) | kind 3 (16:9) | M from 56.327 deg |
|---|---|---|---|
| scoped (presses 1, 4) | 56.327 -> 15.000 deg, ramp 0.8 s | 15.000 deg | 4.07 |
| bare (presses 2, 3) | 56.327 -> 35.000 deg, ramp 0.8 s | 30.000 deg | 1.70 (kind 0) |

- The hip is Settings.xml's `HumanoidFOV` (56.327), the on-foot FOV option. The ship's `FOV` is separate (55.55). Neither changed in the
  flight, so Q3 is not shown. The round ADS values (15, 30, 35) point to absolute FOVs per weapon (INFERRED).
- The zoom's source in flat is the camera's own FOV (+0x280), which the refresh builds the projection from.
- Kind 3 never came back to 56.327 deg after press 1: its maximum is 47.000 deg in every press, and its ramps (15 to 42 s) are stale.
  INFERRED: a second camera at 47 deg while the weapon is out. Open: whether the world is drawn with kind 0 or kind 3. That sets the bare
  copy's M (1.70, or 1.62 from 47 deg) and the hip the scoped M is measured from (4.07, or 3.30).
- Instrument limit: `PressTracker` never commits a plateau that begins before Status.json's late flag falls. So a camera whose resting
  value changes keeps its first hip, as kind 3 did. Its minimum and maximum still show the truth.
- Not covered: the other weapons (only the Executioner was pressed), the FOV option (Q3).
- **Consequence:** the scoped Executioner's M (3.3 to 4.1) is past section 6's 2.5 line for option A. So B3 (the left eye rendered as the
  zoomed view) goes to the maintainer, and the VR S0 flight still decides Q2 (VR) and Q4 to Q6. The bare copy zooms too (1.7), so its sight
  needs the same Q4 answer.

**2026-10-28, S0 VR flight** (Epic in VR mode, `edvr_gfx_20261028_155111`, `d90db147`, build matched). `--health`: S0 vr, geometry and
census all OK. Two presses, the scoped copy then the bare one, both settled (w = 1.0 for 4.2 s and 4.5 s).
- ruled out: RC+0x16C and +0x17C as a VR zoom field, because both read -1.0000 at the hip and in ADS for both copies.
- ruled out: the refresh hook's cameras as a VR zoom field, because none narrowed. The eyes (kind 5, 93.6 deg) held. The only one that moved
  was a square kind 0 camera at 90.5 deg in ADS; its 160.9 deg hip is the stale-hip limit.
- So VR computes no zoom that S0 can see, and M comes from the flat table (section 1). The eyes' p11 held at 0.9390 (the control).
- Q5: the right eye's c sits 0.87 deg (scoped) and 1.27 deg (bare) off that eye's axis, at px (821, 995) of 2016x2017. The left eye's c is
  17 deg off its axis. The game's |E - S| is 28.67 cm scoped against EDVR's 17.68 cm (EDVR keeps the bare depth, 11 cm nearer: F149's +5
  and +16 cm). Bare: 17.97 against 17.97.
- Q4 and Q6, NOT settled:
  - The probe ran in one eye's lit-colour target (its draws: the lighting resolve `7E38A6AA1269C901/7CECABDE34FFBE9E` and the sky
    `F8FA801F2CB1E27C/84965D3C050FB01B`). It declined 1284 draws of other formats (its one atlas takes the first target's format, so the
    G-buffer is never seen).
  - It wrote "eye 0", from `eyeIndexFor`'s first-seen order, which does not name the physical eye.
  - In that eye the sky drew over all four windows, so no opaque depth was there. Additive instanced strips (VS `0357BBB2DEE43C1F`) and an
    alpha UI quad (VS `4EF6DDB075A927FA`, PS `098C0764D28FC42C`, 8 deg down-left) also drew; both differ between the copies.
  - No eye dump was taken. The census rule's OK overstates what this run shows.
- **Next, the same build in one Epic session:** VR, an eye dump (INSERT) in scoped ADS and one in bare ADS (Q4 to Q6 from the picture;
  the dump also re-runs the probe at S0's points). Flat, the other weapons and a `HumanoidFOV` change (Q1, Q3).

**2026-10-28, S0 VR flight 2, the eye dumps** (Epic in VR mode, `edvr_gfx_20261028_173510`, `d90db147`, build matched). `--health`: S0
vr, geometry and census OK (F85c NEVER APPLIED, as on every flight of this build). The scoped copy (held 6.5 s), then the bare one (5.4 s),
both settled. Dumps `173845379001` (scoped ADS), `173855469002` (bare ADS), `173903271003` (the hip, after the release). No flat part.
- Q2 as in flight 1: RC+0x16C/+0x17C -1, no camera narrowed, p11 0.9390.
- ruled out: flight 1's census reading ("the sky drew over all four windows"), because the probe's eye 0 is the left eye and its points
  are the right eye's. The dump's DLSS inputs (eye 0) show the sight at the left eye's c (px 1478), so the probe watched open ground
  17 deg beside it. No S0 census run has looked at the lens; the dump answers Q4 to Q6 instead.
- **Q4 SETTLED, see-through glass** (MEASURED, both copies). In the left eye's depth the sight's frame writes depth (0.14 reverse-Z, about
  18 cm) and the glass writes none: inside the frame the depth carries on the ground's gradient from outside it (scoped 0.00122 at c,
  between 0.00126 left of the frame and 0.00118 right of it; bare 0.00025, a farther hill). The world shows through at true scale under a
  light teal tint. No opaque screen, no picture in the lens. The lens's footprint is therefore the hole in the frame's depth around c (or
  F119's weapon stencil): section 3's fallback is the route, and its opaque-screen case does not arise.
- **Q5** (MEASURED on the final right-eye image, 4032x4034, about 1894 px per unit tangent): the opening is a flat hexagon about 11 x 6 deg
  scoped and 10.6 x 7 deg bare, with c about 1.2 deg (scoped) and 0.8 deg (bare) above its centre. At M 4.07 the scoped lens would show
  2.7 x 1.5 deg of the world. EDVR's scoped eye sits 11 cm nearer than the game's E, so the lens looks 1.6 times larger than from E.
- **Q6** (MEASURED): over the glass the game draws a chevron reticle and the tint, both in the scene colour before DLSS (DlssBeforeUi);
  EDVR's UI mask is 0 over the whole 300 px square around c. The chevron sits at the same offset from c in both eyes, so it is on the glass
  (about 3 mm below S), not at infinity. Its apex reads 0.9 deg (scoped) and 1.2 deg (bare) below c from EDVR's eye, and about 0.5 deg
  from the game's E. A magnified copy (A) magnifies it with the world, as flat's zoomed camera does, and from EDVR's nearer eye it lands
  about 3.7 deg below c, near the lens's lower edge; drawing it at true scale instead would need its draw named. No HUD or readout part
  lies over the glass.
- Instrument limits for any later census: the probe watches one eye (the first-seen, the left here) and one target format. Points must be
  per eye.
- **Next:** flat only, the same build: the other weapons and a `HumanoidFOV` change (Q1, Q3, kind 0 or 3). A or B3 is the maintainer's
  call now; Q4 removed nothing from A, and M 4.07 is what argues for B3.

**2026-10-29, S0 flat flight 2, every weapon** (Epic in flat mode, `edvr_gfx_20261029_052412`, `d90db147`, build matched). 38 presses:
each owned weapon three times, the Executioner scoped (presses 33 to 35), then bare (36 to 38). Between presses 3 and 4 the hip moved
from 56.327 to 64.752 deg: the on-foot FOV option, which Settings.xml now holds as `HumanoidFOV` 64.751938. The values are kind 4's,
whose tracker caught every hip; kind 0 settled at the same value in every press.

| ADS, the world (kind 0 = kind 4) | Items (Status.json `$wpn_..._name`) | Kind 3 in ADS | Ramp | M at 56.327 | M at 64.752 |
|---|---|---|---|---|---|
| 52.000 deg | `m_shotgun_plasma_doublebarrel`, `s_pistol_plasma_charged`, `s_pistol_kinetic_sauto`, `m_launcher_rocket_sauto`, `s_pistol_laser_sauto` | 30 deg | 0.15 to 0.65 s | 1.10 | 1.30 |
| 50.000 deg | `m_submachinegun_laser_fauto`, `m_submachinegun_kinetic_fauto` | 30 deg | 0.45 s | 1.15 | 1.36 |
| 48.000 deg | `m_assaultrifle_plasma_fauto`, `m_assaultrifle_laser_fauto`, `m_assaultrifle_kinetic_fauto` | 30 deg (laser 20) | 0.27 to 0.45 s | 1.20 | 1.42 |
| 35.000 deg | `m_sniper_plasma_charged`, bare | 30 deg | 0.8 s | 1.70 | 2.01 |
| 15.000 deg | `m_sniper_plasma_charged`, scoped | 15 deg | 0.8 s | 4.07 | 4.82 |

- **Q3 SETTLED, absolute** (MEASURED): with the option at 64.752 deg the scoped Executioner settles at 15.000 deg and the bare one at
  35.000 deg, as at 56.327 deg on 10-28. The game keeps an ADS FOV per weapon (and scope), not a factor, so M is that FOV against
  whatever the hip is; section 4 has the consequence for VR.
- **Kind 0 is the world's camera** (INFERRED, strong): kind 0 and kind 4 followed the option from 56.3 to 64.75 deg, while kind 3's
  maximum stayed 47.000 deg in all 38 presses. Kind 3 is the weapon's own camera, at a fixed 47 deg. So the scoped M from 56.327 deg is
  4.07, not 3.30.
- **Q1 SETTLED for the owned weapons** (MEASURED): every one narrows the world in ADS, to 48 to 52 deg, all past section 4's 1.05 line.
- Two presses read the previous weapon's value, each the first press after a switch: 19 (`m_submachinegun_laser_fauto` at 48, the
  assault rifle's) and 29 (`m_submachinegun_kinetic_fauto` at 52, the launcher's). Each weapon's own value is its other presses'.
- Instrument limit, as on 10-28: kind 0's hip field is stale from press 16 (56.249 deg, with ramps of 5 to 30 s); its maximum and kind
  4 read 64.752.
- Separately, the maintainer saw the scoped Executioner stutter on ADS in flat, and the bare one not; that has its own investigation
  doc (Status block).
- **Next:** no flight for the table. The maintainer decides A or B3, scopes only or every sight, and M's reference FOV (section 4).

**2026-10-29, the maintainer's decisions:** A first, for every sight; B3 later and only if the scope is too soft. The reference FOV
starts at 56.3 deg, set by an experimental menu row. The build is A1 (section 8).

## 8. A1: the first build of option A

**Behaviour.**
- Keys (section 5): `fix.native_on_foot_ads_scope_zoom` = on | off; `experimental.native_on_foot_ads_scope_zoom_fov` = the reference FOV h.
- The lens runs in native on-foot VR when all of these hold: in ADS; the game's w at 1.0 with S fresh (S0's `geometryNow` conditions); the
  held (item, scope) in the table; M >= 1.05. Otherwise there is no lens, and the frame is counted under its reason. An unknown scope
  state counts as "not in the table".
- The table holds the ADS FOV per (item, scope), from section 7. Items match on the stem, so `$wpn_m_sniper_plasma_charged_name;` and
  `wpn_m_sniper_plasma_charged` are one:
  - `m_sniper_plasma_charged`: scoped 15.000, bare 35.000.
  - 48.000: `m_assaultrifle_plasma_fauto`, `m_assaultrifle_laser_fauto`, `m_assaultrifle_kinetic_fauto`.
  - 50.000: `m_submachinegun_laser_fauto`, `m_submachinegun_kinetic_fauto`.
  - 52.000: `m_shotgun_plasma_doublebarrel`, `s_pistol_plasma_charged`, `s_pistol_kinetic_sauto`, `m_launcher_rocket_sauto`,
    `s_pistol_laser_sauto`.
  - Every other item, and every other scoped copy, is unmeasured: no lens.
- M = tan(h/2) / tan(ads/2). It eases from 1 to M over 0.25 s, from the first qualifying frame of a press. It drops to 1 on the first
  frame that does not qualify.
- **Where:** the right eye's picture at the door, after RCAS. It goes right after `rippleDoor` in `uiLayerComposite`, before the UI
  composite, and takes the frame through the same route (owned UAV, or the copy for a frame that is not EDVR's; R8G8B8A8 or B8G8R8A8
  UNORM only). F106 then carries it to the left eye; with F106 off the left eye stays unmagnified.
- **c:** the right eye's c from S0's `geometryNow` math, worked out each qualifying frame on the frame thread and corrected for that
  eye's draw jitter (the output is unjittered). It is published with M and a sequence by seqlock (the `EyeRowsPub` pattern). The door
  uses a c no older than 50 ms.
- **The footprint is the hole in the depth** (Q4). Pass 1 is a compute pass that marches 128 rays from c over the right eye's
  render-res depth (the game's depth, viewed in place, with the DSV unbound around it). Each ray stops at the first run of 3 render px
  nearer than 0.6 m (metres from the eye's projection rows), or at 12 deg. More than 2 rays reaching 12 deg means an open sight: no lens
  that frame. Pass 2 runs over the box of the largest ray: out(p) = in(c + (p - c) / M), Catmull-Rom from a region copy, inside r(theta)
  (interpolated between rays) less a 2 output px feather. The scaling is linear in output px about c: the eye image is planar in
  tangent, and c is within 2 deg of the axis. Nothing is read back to the CPU except the instrument's rays, asynchronously.
- Cost, to be measured: 128 x 200 depth loads and a copy of at most 800 x 800 px a frame, in scoped ADS only.

**Instrument** (tag S1; `--health` rules):
- `scope zoom (S1): on|off, reference FOV h deg, N table entries`, once at arm and on every key change.
- One line per press: item, scope, ads, target M, h; frames applied of qualifying frames; frames declined by reason (no S, stale c, open
  sight, not in the table, depth unavailable, format refused); the hole from one settled frame's rays (r min, median and max in deg, and
  the rays that escaped); the spread of c in output px; GPU ms of each pass (a `GpuTimer`, as sharpen does, and a GPU census door
  section).
- Health: NEVER APPLIED when a press qualified with the key on and no frame applied; PARTIAL when under half its qualifying frames
  applied; IDLE with no press.
- If the code never runs, there is no announce line, so `--health` can tell that from a run with no press.

**Rigs:** the pure parts (the table, M, the ease, the gate, the open-sight rule, r(theta), the mask, the source mapping) in a header
with `tools\native_frame_test\s1_checks.h`. A GPU rig in the pattern of `tools\shield_ripple_test` runs both passes on a synthetic frame
whose depth is a near hexagonal ring about c, far inside and out. It checks that inside the ring the frame is sampled at c + d/M, that
the ring and everything outside are unchanged, that an open ring leaves the frame alone, and that another format is refused.

**As built** (2026-10-29, branch `claude/scope-zoom-a1`; the full local build and all its gates green, nothing flown; the review fixes below are the second commit).
- Pure parts: `src\d3d11\scope_zoom_s1.h` (table, M, gate, ease, the reference march, r(theta), mask, window, the rays buffer's layout, the accumulators and the two
  lines), checked by `tools\native_frame_test\s1_checks.h`. The passes: `src\d3d11\scope_zoom_shaders.h` (registered in
  `tools\temporal_shader_build\fixed_scope_zoom_variants.h`) and the door, `src\d3d11\scope_zoom.cpp`, linked as shipped by the GPU rig
  `tools\scope_zoom_test` (WARP, debug layer; R8G8B8A8 and B8G8R8A8 in place, a copy for a frame EDVR may not write, a gap of two rays, an open sight, a refused format,
  every decline reason, three mutants of the comparator). The frame thread's half is `s1::tick` in `native_on_foot.cpp`, after `s0::vrTick`.
- The depth is the view the temporal pass makes for the upscaler (`scopeZoomDepthSet` in `temporal_pass.cpp`, cleared at the top of each eye's treat), so the door
  reads exactly what DLSS read, and the output merger's depth target is unbound around pass 1. Only the right eye's door (Submit's eye 1) is called. The pass tells the lens whether
  the Submit region is the whole source and the depth the source's size; a depth that does not cover the render is `depth unavailable`.
- c is published as a uv of the render with the draw's jitter in uv; the door puts it on the region (c less the jitter, unjittered) and on the depth (as drawn). A c older than
  50 ms is `stale c`. The region must be the whole texture, in place or on the copy route (a sub-rectangle is `format refused`: its offset is not in c). The window the passes touch is
  the 12 degrees about c (at most ~1000 px square), so the region copy does not grow with the output.
- **c is the far impact point, not S** (review fix 4). The game's shots go along the line E -> S (F143: E the eye matrix's position, S the sight point, one F141 sample), so the
  lens projects the point 100 m along S - E past S through the right eye's rows, and keeps c through S only for the line. `fwd` in S0's `geometryNow` is a dummy: S is projected at distance
  0, where the direction does not matter, so it is not the line's direction and was not used; the direction is S - E from `s0::g_vr`. The press line prints the angle between the two, mean and
  max. S0's geometry line is unchanged (`geometryNow` only keeps the rows it projected through).
- **The jitter is taken at the draw** (review fix 3): `eyeRowsPublish` reads `nativeTemporalDrawJitter` for the right eye as the rows are published (the render thread, the frame
  the channel's beginFrame opened), with that frame's temporal sequence, while the key is on. The tick counts qualifying frames with no reading and with a reading from another frame than
  the rows; the door counts frames whose c was drawn in another sequence than the picture's (`c from another frame than the picture`) and applies the lens all the same.
- **Counts that add up** (review fixes 1 and 2). A frame at M = 1 has nothing to do and is counted as `at M 1`, after every decline the door can make, and is not applied; `applied` is
  the dispatched frames less the open sights. The press line also carries the frame thread's own counts of the gate's verdicts (qualified, no S, not in the table, idle), so a door
  that is never reached shows as qualified N, qualifying 0.
- `--health` (review fixes 1, 2 and 5): a press is due on the larger of the tick's qualified + no S and the door's qualifying + no S, less the M = 1 frames. Presses due with the lens applied on
  none: NEVER APPLIED (the text says when the door counted no frame at all although the tick qualified). A press in which every ray read back said open is no failure: the verdict
  `OPEN SIGHT` when it is all there is to judge, a note beside an OK otherwise.
- Hole statistics (review fix 6): every ray that escapes (the 12 degree stop, off the image, the step cap) stores 12 degrees, so none is a hit in r min, median and max.
- A press still open when the tick finds the profile flat or the install not armed gets its line (review fix 9). Leaving native on foot was already closed: `ads` includes on foot.
- Departures from the plan above, each on purpose: (1) an escaped ray (at most two; more is an open sight) takes the smaller radius of its nearest rays that did not escape, so a
  gap in the frame does not draw a spike of lens; (2) the open-sight rule runs on the GPU (pass 2's groups are 0) and the CPU learns it from the rays read back, a few frames
  late, so `open sight` and `applied` lag by the ring and rays not yet read count as applied; (3) `not in the table` also counts an M under 1.05, and `format refused` also counts
  the other ways the frame route can refuse (a picture that is not the upscaler's, a region that is not the whole texture, a view, a copy, a shader); (4) the user key is off by default
  ("default on once flown OK"); (5) the experimental key's row is in the developer tier, where the generator puts every `[experimental]` key (the section decides the page); (6) the GPU
  census item is named `lens` in the census line, to keep its worst-case line under the 1150 characters `gpu_census_test` holds; (7) the ease is a smoothstep of the exponent, so the
  zoom grows evenly in size.
- Not verified, for the flight: that the depth is intact at the door (the S1 line says: `depth at c` far, `hole r` 4 to 8 degrees for the Executioner, `depth unavailable` 0);
  the eye numbering (S1's `c mean` for the scoped Executioner should be near S0's geometry px (821, 995) of 2016x2017, that is about (1642, 1990) of the 4032 picture, less the
  jitter, and further by the far point's offset from c through S, which the line prints); that the bounds the door gets are never flipped (the lens maps the eye's uv onto the region as
  given); whether c from the frame tick belongs to the picture's frame (`c from another frame than the picture` says how often not); the raw indirect-argument buffer and the typed
  stores on a real GPU (the rig ran on WARP).
**Flight card (A1, Epic in VR):**
- Turn "ADS: scope zoom" on in the Native on foot page first (the key is off until flown).
- The scoped Executioner in ADS: 10 s still, then 10 s turning the head.
- The bare Executioner, and two unscoped weapons (a 48 deg rifle and a 52 deg pistol).
- In one scoped ADS: switch the key off and back on, and move the reference FOV row to 64.8 and back (the row is on the Experimental
  page, which shows with the menu's Developer mode on).
- Read `edvr_log.py --expect-build 5a47ad8f --health` first, then the S1 lines.

**2026-10-29, A1 VR flight** (Epic in VR, `edvr_gfx_20261029_093933`, `5a47ad8f`, build matched; the key on from 09:40:56). `--health` exit 3 for
F85c only, as on every flight of this line; `scope zoom (S1)` PARTIAL, 19 presses. Dumps `094209717001` (scoped Executioner, ADS), `094230243002`
(bare, ADS), `094507077004` (Oppressor, ADS), `094606467006` (Aphelion, ADS); 003, 005 and 007 near a release.
- **Applied wherever the sight is a closed frame:** the scoped Executioner 1846 of 1865 frames (M 4.06), the bare one 971 of 987 (M 1.70), the
  M Oppressor 971 of 986 and the TK Aphelion 707 of 729 over three presses (M 1.20). Every other weapon (Tormentor, AR-50, P-15, Eclipse,
  Intimidator, C-44, Zenith, L-6) was an open sight on every frame read: 26 to 108 of 128 rays escaped.
- **Verified:** the depth is intact at the door (`depth unavailable` 0 on every press, `depth at c` 0.00016); the eye numbering (the scoped
  `c mean` (1658, 2013) against this flight's S0 geometry, px (832, 1005) of 2016, that is (1664, 2010)); `hole r` median 4.1 deg scoped, 4.4 bare,
  3.0 Oppressor, 2.4 to 4.7 Aphelion; GPU: rays 0.03 to 0.11 ms, magnify 0.017 ms.
- **Unknown:** `c from another frame than the picture` on every applied frame. The count does not say by how many frames. c moved 2 to 7 px
  over a press, so in ADS the sight holds still on screen and a lag of one frame would move the lens little. The next build logs the gap.
- **The pilot:** "a really cool effect where it makes sense". Two issues:
  - (1) The centre chevron is "incredibly distorted" with the scoped Executioner, less with the bare one. The dumps show why, as Q6 said: the
    chevron is in the scene, so it is magnified with the world about c. A copy M times its size lands at the lens's lower edge: a teal wedge
    behind the pink bars (scoped), a white roof across the bottom (bare). The pilot asks for the cyan-green scope marks at true size over the
    magnified picture.
  - (2) The zoom makes sense only with an optical scope attached; iron and holo sights get none. The pilot's reference is a community chart
    (flat, 150 m, every weapon without and with the scope). Read side by side: with the scope fitted, the Executioner magnifies strongly and
    the Aphelion a little. The Eclipse, Oppressor and Zenith look the same either way. The Karma weapons, Intimidator and Tormentor get a
    reflex or holo sight.
- ruled out: the chevron in EDVR's holo or UI layer, because in dump 002 the holo coverage is empty over the sight and the UI mask is 0 (as Q6).

## 9. A2: optical scopes only, the scope's marks at true size

**The gate (the maintainer, 2026-10-29).** Iron and holo sights get no zoom. The bare Executioner and the Oppressor have optical sights and
zoom. The unscoped Eclipse does not, and its scope mod fits the Aphelion's base scope. The table holds optical sights only, keyed by (item,
scope):
- `m_sniper_plasma_charged` (M Executioner): bare 35.0 deg and scoped 15.0 deg, both measured.
- `m_assaultrifle_plasma_fauto` (M Oppressor): bare 48.0 deg measured; scoped unmeasured (the same frame in the chart).
- `m_assaultrifle_laser_fauto` (TK Aphelion): the base sight is the ring scope. Bare 48.0 deg measured; scoped unmeasured.
- `m_submachinegun_laser_fauto` (TK Eclipse): scoped only, the Aphelion's base scope. Unmeasured.
- An unmeasured scoped copy takes its bare copy's ADS FOV as a stand-in (the chart shows the scoped Eclipse's and Oppressor's targets at
  their bare size), and the press line says so (`FOV from the bare copy`), so one flat S0 press can replace it.
- Every other copy has no entry and no lens: the Karma weapons (iron sights bare, reflex scoped), the Zenith, the unscoped Eclipse, and the
  Intimidator and Tormentor (open bare; their scoped frames not yet classed). Their FOVs stay in section 7 as data. The press line's `not in
  the table` becomes `not an optical sight`. The open-sight rule stays as a guard.
- In A1's flight the lens ran on exactly the bare and scoped Executioner, the Oppressor and the Aphelion, so A2 changes no sight that flew;
  what it removes is a lens on a closed reflex frame (a scoped Karma weapon) that A1 would have drawn.

**The scope's marks at true size (the pilot: the cyan-green marks over the magnified picture, at their own size).**
- **The draw (named by research, INFERRED for the chevron).** The readout's one world-space part (space 0, `+0x19` 1; parts 29232, 29959
  and 29221 for the scoped Executioner in A1's log) is the AR gunsight glyph, bound at the `Ui_AR_*_ADS_Gunsight` locator. It is drawn as a
  quad, 6 indices and 1 instance, by VS `025B4B9FF54622ED` / PS `46F92DC71BF8DFA5`, sampling the readout atlas (3525x3133; `DC #3914` in
  the census of `edvr_gfx_20261028_173510`), into the eye's scene HDR target before DLSS, with no depth write. The same pair's issued draw,
  read before and after, adds the rifle's white dot (`design-native-onfoot-vr-2026-10-13.md`, the rifle dot). Nothing in EDVR moves it:
  `f85bTagSubmit` returns for any part not in space 1, and the layer census lists the pair as "no family", about 1.35 draws a frame.
- **The key must be narrower than the pair.** Flat's weapon footprint uses the same pair as the weapon and tool material
  (`flat_weapon_footprint.h`). The mark is the pair drawing 6 indices, 1 instance, with the readout atlas bound (the texture the space-1
  parts sample).
- **The route: a UI layer over-blend family.** These families take an HDR scene draw out of the scene and redraw it into the eye's layer,
  which `compositeInner` lays over the picture after the magnify (`uiLayerComposite`: `rippleDoor`, then the lens, then `compositeInner`).
  Not the weapons-fire layer: it is additive only, and the mark is alpha-blended. The family is armed only while the scope zoom key is on,
  on foot in VR, ADS with a held (item, scope) in the optical table, so no other sight changes. A frame that redirects and then declines the
  lens still shows the mark where the game drew it, so a frame of lag at a press's edges costs nothing.
- **Jitter.** The mark must land where the unjittered DLSS output has it. Whether this pair's matrices carry the eye's jitter is open (the
  weapons-fire table records it per pair). The build reads how the over-blend families treat jitter, cancels it for this pair if they do
  not, and says which in the family's first-seen line.
- **The tint stays in the scene** (the glass's own draws, PS `C5A5C7E8216CB9AF`, run before the quad) and is magnified. A flat tint looks
  the same either way. Its honeycomb at the glass's top edge would grow M times; it moves after the door next, if the pilot wants it.
- Unchanged: the mark sits 0.9 deg (scoped) or 1.2 deg (bare) below c from EDVR's nearer eye (section 7, Q6). That is F150's eye depth.

**Instruments.**
- **The probe on the left eye's own points.** It watches the first-seen eye, the left one in every logged frame, so S0's census gives it
  the left eye's c. Points: c, 0.9 deg below (the chevron), 1.5 deg above (glass only), 4 deg beside (the frame). Its line prints the
  probe's eye beside the physical eye. With the family armed, the quad should change no window in the scene.
- **The family's counts** in the S1 press line: marks redirected per eye, and armed frames with no matching draw.
- **The gap between the picture and c:** the picture's sequence less c's, as a histogram (-1, 0, +1, +2, other).
- The press line names `not an optical sight` and `FOV from the bare copy`; `--health` reads both.

**Flight card (A2, Epic in VR, the key on):**
- The scoped Executioner in ADS: 10 s still, then 10 s turning the head, with an eye dump (INSERT) while still.
- The bare Executioner in ADS, with a dump. Then the Oppressor and the Aphelion in ADS.
- One Karma weapon in ADS: no lens, and the reflex sight unchanged.
- Read `--expect-build ce3f9bac --health` first, then the S1 lines and the probe's lines.

**As built (A2)** (2026-10-29, from `114c05ba`; the full local build and all its gates green, nothing flown).
- **The table** (`scope_zoom_s1.h`): four rows, 7 entries. The Executioner bare 35.0 and scoped 15.0; the Oppressor and the Aphelion bare 48.0, their scoped copies standing in at
  48.0; the Eclipse scoped only, standing in at 50.0. `lensAdsDeg` says when the FOV is a stand-in and the tick counts those ticks (`FOV from the bare copy`). The verdict is
  `kNotOptical` and the press line's decline reads `not an optical sight` (an M under 1.05 is counted there too). The open-sight rule is untouched.
- **The family** is `UiLayerFamily::kScopeMark`, last in the enum. `uiLayerFamilyOf` (vscreen.cpp) names it when the target is the lit HDR one, the vertex shader is `025B4B9F...`,
  the draw is indexed with 6 indices in 1 instance, `scopeZoomMarkArmed()` holds, the pixel shader is `46F92DC7...` and a learned interface surface is bound. The cheap tests come first, so
  an ordinary draw reads no hash. `scopeZoomMarkArmed()` reads the published frame as the door does: verdict `qualifies` (the key on, native on foot VR, ADS, w at 1.0, an optical
  sight, S and c fresh) and no older than the door's 50 ms.
- **The atlas is found by what it is, not by its size:** the texture at PS slot 0..3 that ui_depth has learned as an interface surface (a texture the GUI renderer draws into).
  The readout composite (B10B/DB89) samples that surface (ui_depth's first-seen line for it says `samples a learned surface`), and so does the same line for 025B/46F9 in the
  2026-10-28 log; the material's textures in that census are 1024x1024 BC-compressed (DXGI formats 70 and 83) and never a GUI target. The 3525x3133 size is printed in the family's first-seen line, with the slot
  (the census had it in slot 2), so the flight checks the identification; no code depends on the size.
- **The route** is the generic HDR HUD take: the draw goes into the eye's HDR layer, the game's tonemap draw re-issued over it publishes the 8-bit layer, and `uiLayerComposite`
  lays that over the picture after the lens (`scopeZoomDoor` for Submit's eye 1, then `compositeInner`; scope_zoom_test pins the order). The left eye takes the same route into its
  own layer with no lens; F106 then covers the left eye from the right eye's finished picture (in the `both` mode of `fix.native_on_foot_ads_sight_eye` only, F162), so the mark in the
  covered part is the right eye's, at true size. With `right` each eye keeps its own view and the left eye's mark is its own redirected one.
- **Departure, the blend.** The census blend is ONE, DEST_ALPHA (`bl=12,7,1/2,7,1`), not an over. `uiLayerBlendShape` refuses destination-alpha factors, so the pair would have been
  refused. The target is R11G11B10_FLOAT, which has no alpha: the output merger reads destination alpha as 1 there (reproduced on WARP by sharp_weapons_test's
  `destAlphaReproduction`), so the draw adds the mark's light, as ONE, ONE does. The family's blend is read that way (`uiLayerBlendDestAlphaOne`, this family and a format without
  alpha only), at the decision and at the issue alike, and the layer takes it as an additive draw: light added, transmittance untouched.
- **Departure, the governing half.** The family follows fix.ui_quality's half, as the other HUD families do, so with the layer off the mark stays in the scene. The press line shows
  it (`gunsight marks redirected: L 0, R 0` beside armed frames), and `--health` calls it PARTIAL with that reason.
- **Jitter.** Every non-weapons family cancels the eye's jitter: `uiLayerDecide` sets `g_draw.jx = jx` unless the weapons-fire table marks a pair unjittered, and this pair is not in
  that table, so it needed no entry. That the pair's matrices carry the jitter is INFERRED, and the first citation of it was wrong (corrected in the review fixes below):
  flat_projection_recipes.h:68 is keyed by (VS 025B, PS C5A5C7E8216CB9AF), the glass tint's pair, which puts the camera at b1 row 270, the scene camera rows the weapons pairs' matrices
  were shown to equal (weapons_fire.h, W3); the mark's PS 46F9 has no recipe. The inference rests on the mark sharing VS 025B and so reading its camera from the same constant-buffer
  row. The family's first-seen line says the cancel is inferred, not shown. If the eye dump puts the mark off c by the jitter, the pair goes into a table as the weapons-fire pairs do.
- **Instruments.** The S1 press line carries `not an optical sight`, `FOV from the bare copy`, the histogram of the picture's sequence less c's (`-1, 0, +1, +2, other`; A1's count of
  c from another frame is every bucket but 0, and `--health` prints it as such), and `gunsight marks redirected: L, R; armed frames, no mark draw`. A mark is counted where the layer
  takes the draw. The door counts a right-eye frame as armed when the verdict qualifies, and as without a mark draw when no mark was redirected in the picture's own sequence. The
  worst-case press line (every counter six digits) is 979 bytes; the rig holds it under 1100. The S0 census uses the left eye's rows and prints `eye L` and `probe eye 0 is physical
  eye L`; its points are c, 0.9 deg below, 1.5 above and 4 beside.
- **If it did not work, the log says which way.** The family never named (the shape, the armed test or the atlas test failed, or the layer is off): `gunsight marks redirected: L 0, R 0`
  with armed frames above 0, and no family line. Named and declined: the same counts, and the family census names the decision (blend, depth-stencil, not armed). The door never
  reached: armed frames 0 beside the tick's `qualified`, the existing `door counted no frame` text. Applied frames with every bucket of the histogram at 0 are the frames with
  `jitter reading none`.
- **Not verified, for the flight.** (1) The identification: the first-seen line should show the atlas, 3525x3133, in slot 2. (2) The decision: the draw tests depth (GEQUAL, no write)
  and stencil (ref 4), which the layer reproduces from a seeded copy of the game's depth-stencil; the family census says if it was redirected, and the cost of that seed in ADS is
  unmeasured. (3) The brightness: the layer tonemaps the mark's light alone and adds it, where stock tonemapped it with the scene. (4) The jitter, by the eye dump: the quad should
  change no window in the scene and sit on c. (5) A draw the layer did not take that stock drew after the mark now falls under it (the HDR phase's known limit). (6) `armed frames`
  against `no mark draw` in a settled press: near 0 is the pass. (7) That the quad is the chevron at all: the probe's four points on the left eye (the chevron's is 0.9 below c).

**As built (A2 review fixes)** (2026-10-29, on A2 `cf292810` merged with main `562759c0`; the full local build and all its gates green, nothing flown). A review of A2 found eight
things, and the merge a ninth. Each, in one line:
- **`--health` and a mark left in the lens eye.** A due press with armed frames is PARTIAL when the right eye's redirects are under 80 percent of them (none in either eye
  included, and the left eye redirecting alone) or over 1.5 times them (more than one draw a frame matched: the match is too wide); the verdict names the counts. Rig: R 0 of 310 with
  L 300, R 3, R 247 of 310 PARTIAL; R 600 PARTIAL; R 298, 248 and 465 of 310 OK.
- **The vacuous rig check.** The one-bit vertex-shader mutants of the family are now built with the flag set, so they can differ from the real thing in the vertex shader alone. The
  rule the hook asks became a function, `uiScopeMarkFacts` (ui_layer_math.h), which vscreen.cpp calls with the real reads and sharp_weapons_test drives with stand-ins: 5 and 12 indices,
  2 instances, a draw that is not indexed, the lens not armed, PS C5A5C7E8216CB9AF, no learned surface each refuse alone; all together name the family; a refusal stops before the reads
  after it. scope_zoom_test keeps only the wiring (the hook hands the rule its three real reads).
- **The jitter citation.** Corrected above and in the code: the recipe at flat_projection_recipes.h:68 is for (VS 025B, PS C5A5C7E8216CB9AF); the inference is the shared vertex shader.
  The family's first-seen line says the cancel is INFERRED, not shown.
- **The depth decision, for the mark alone.** The layer seeds its own depth-stencil copy from the game's TEXTURE (shader views over it, or a copy; never through the game's view) at the
  layer's size with the frame's jitter cancelled, and binds it writable with the game's own depth-stencil state; `dsReproducible` refused any view with a flag set because a writing draw
  against a read-only view would change the copy. The mark's draw tests depth GEQUAL and stencil (ref 4) and writes neither. Now: (a) if the state cannot write (depth write off or depth
  off; no stencil operation that changes the stencil under a write mask: the state's potential, whatever the view's flags say) a read-only view is accepted when the rest of the
  reproduction holds (format, size, the seed available): path `seeded read-only`; (b) a test the layer cannot reproduce for any other reason is dropped, and the mark is redrawn in the
  layer with the depth and stencil tests off (no depth target bound), because it lies on the sight glass in front of the world, inside the lens, where nothing nearer is drawn:
  path `tests off`; (c) a mark that tests AND writes and cannot be reproduced keeps the ordinary refusal. Each redirected mark is counted by path in the press line (`mark depth
  path: untested, seeded, seeded read-only, tests off`), and the first-seen line names the path of the first (with the reason for tests off).
- **Identification evidence on a decline.** The first-seen line of every decision of the family carries the readout atlas by size and PS slot (or says no learned surface resolved),
  the game's blend, and, once the depth decision was made, the path. `not armed` gets a first-seen line for this family (the other families keep theirs transient); late and not-UI do
  not. The detail buffer grew from 320 to 640 bytes (the old redirect text could be cut before the atlas). No new per-press line: the S1 press line is capped at 200 a session.
- **The game's blend, not the rewritten one.** `shapeOf` takes a fourth output, the game's own factors; the declines print those (`bl=12,7,1/2,7,1` for ONE, DEST_ALPHA) and the
  first-seen line says where the target's lack of alpha read destination alpha as 1. The layer still converts the rewritten blend. Rig: `shapeOf` on a real blend state on WARP.
- **edvr.ini.** The scope zoom key's text says the mark stays at its own size over the magnified picture only while UI quality is on (any value but off; the default is 100) and that it
  stays in the scene, magnified, without it; `--health` says so. There is no separate crisp UI option: the crisp HUD take comes with UI quality and stands down only on a failure of
  its own, which the log names.
- **Armed frames after the freshness test.** The door counts a frame as armed, and as with no mark draw, after the stale-c test; a stale-c frame is neither. Rig: a stale c is counted
  stale and not armed.
- **The left aiming eye (the merge).** Main's F162 replaced F106's key with `fix.native_on_foot_ads_sight_eye = both | right | left`: the aiming eye is the right eye for `both` and
  `right` and the left eye for `left`; F106's share runs in `both` only. The lens is the right eye's (the door's `eye != 1`, the right eye's depth, scene rows and jitter), so the gate
  has a verdict, `kLeftAim`, after the idle tests and before the optical-sight test: the press line counts `left aiming eye` (the tick's and the door's), the lens does nothing, and the
  gunsight mark's family is not armed (it is armed on `qualifies` alone). `--health` calls it no failure: the press is not due, and the verdict text says the left eye aiming gets no
  lens and that a later build may support it. Supporting the left eye is a later build: it needs the left eye's depth slot, scene rows and jitter in the door and the lens. The other
  uses of the right eye in S0 and S1 are the lens's own (the rows `g.eye[1]`, the jitter of eye 1, the door's eye 1) and mean what they did; the sentence in the A2 route paragraph
  about F106 covering the left eye is now true of `both` only. The merge also moved the scope zoom FOV key in edvr.ini behind main's crouched ADS keys: the Hotkeys page rig wants
  the first of them right after `ADS eye forward / back`.
- **The press line** is now written from a buffer of 1160 bytes (`kLensPressLineBytes`): the worst case, every counter six digits, is 1119 bytes, inside the log's 1200-byte line with its
  stamp and truncation marker reserved.
- **Not verified, still for the flight.** Everything in the A2 list above, and: which depth path the mark takes (the press line's counts and the first-seen line say); that the
  seeded read-only path reproduces the game's test (the mark's position against the sight glass is the check, with an eye dump); whether the mark then sits on the glass with its
  tests off (the same dump); the left eye aiming (a press with `left` should show `left aiming eye` and no lens, and no mark redirected, so nothing in the scope is magnified).

**2026-10-29, A2 VR flight** (Epic in VR, `edvr_gfx_20261029_120218`, `ce3f9bac`, build matched). `--health` exit 3: F85c as always; F91 (every readout
part read mesh 0.00 x 0.00, the icon table's luck, the native on-foot arc's; A1's flight already had 6 such parts) and F87p (a prompt came up this time).
Neither is the scope work's. `scope zoom (S1)` OK over 16 presses. Dumps `120515630001` (Oppressor), `120719527003` (scoped Executioner),
`120858916005` (Aphelion), `120958373007` (Karma P-15), `121016259008` (unscoped Eclipse); 002, 004 and 006 at the hip or between presses.
- **The lens and the mark:** applied on 6971 of 7041 qualifying frames; marks redirected L 7007, R 6994 against 6985 armed frames (one a frame
  an eye), none missing on 3; depth path seeded on all 14001 (no read-only view, no test dropped). Every non-optical press (shotgun, Tormentor,
  AR-50, P-15, the unscoped Eclipse) declined as `not an optical sight` on every frame, with no ray, lens or mark.
- **c is one frame older than the picture on every frame** (the gap histogram: +1 on 6985, nothing else). In ADS the sight holds still on
  screen, so the lens moves little; not yet changed.
- **The pilot:** "the chevron looks much clearer now". The dump agrees: in 003 the chevron sits at true size just under c, on the magnified
  drone. Two points:
  - The P-15's sight looks blurrier than remembered. The log shows nothing of the scope work ran on it (1069 frames declined at the gate, before
    any GPU work); no earlier VR dump of the P-15 in ADS exists to compare. Star glow (main's `562759c0`) adds choices only; the live value is 200.
  - The marks are brighter than stock. Cause (from the code, and the arithmetic): the mark goes through the HUD's over-blend family, which
    tonemaps it alone and lays it on the finished picture, T(F) + T(M). Stock adds it in HDR first: T(F + M). Over the bright glass,
    T(F) + T(M) is well above T(F + M). The quad's blend (ONE, DEST_ALPHA on an alpha-less target) is additive, which is the sharp-weapons
    layer's case, and that layer already matches stock by the tonemap difference (`dlss-blur-elements-2026-10-14.md`, "A2, the brightness fix
    (matched)", flown).

## 10. A3: the marks at stock brightness

- **The mark moves to the additive weapons layer.** kScopeMark joins the weapons-fire route: the eye's additive HDR layer, composited after the
  upscale and after the lens by the tonemap difference, T(F_up + L) - T(F_up), with the game's own tonemap re-issued. Its gate stays as A2's
  (the key, native on foot, ADS with an optical sight; the quad's shape and the learned surface), as do its jitter cancel and its depth paths.
  The HUD over-blend family stops taking it.
- **Nothing for sights that are not optical:** the S0 census (the pixel probe) runs only for an optical sight's press. S0's own per-press reads
  are CPU only and stay until the arc closes.
- **Instruments:** the press line counts the marks the weapons layer took and its tonemap-difference composites for them; `--health` checks
  the marks reach that layer.
- **Flight card (A3, Epic in VR):** the scoped Executioner and the Aphelion in ADS, an eye dump in each, judged against stock: switch "ADS:
  scope zoom" off in the same press for the stock chevron (the lens goes too), then on. Then the P-15 in ADS with the key off and on: if its
  sight looks the same both ways, the scope work is not what blurs it.

**As built (A3)** (2026-10-29, from `1094408d`; the full local build and all its gates green, nothing flown).
- **The route.** `uiLayerFamilyUsesWeaponsLayer` (ui_layer_math.h) is the one list of the families drawn into the eye's additive weapons layer W: the weapons fire and the scope mark. For the mark `uiLayerDecide`
  asks `weaponsTakeReady` (W, the 8-bit layer E, the tonemap states) in place of the HUD's `crispTakeReady` and sets `g_draw.weapons`, so `beginInner` draws it as it draws a weapons-fire draw: into W, tested against
  the HUD's seeded depth-stencil copy, through the viewport remap and the jitter cancel (the weapons-fire table does not name vertex shader 025B, so its default, cancel, applies: the same inferred cancel as A2).
  Everything A2 decided is untouched: the match (shape, learned surface, pixel shader), the arming (`scopeZoomMarkArmed`), the three depth paths, the destination-alpha rewrite, the counters and the first-seen
  evidence. Neither the tonemap re-issue nor the composite changed: they read W by its draw count, so a frame with a mark and no weapons fire gets the weapons-only plan (clear E, source pass, base, lit) and
  E = T(F_up + M) - T(F_up), with F_up the scene without the mark. The HUD layer takes no mark now.
- **After the lens, in both eyes (read in the code).** `uiLayerComposite` runs `rippleDoor`, then the lens for eye 1, then `compositeInner`, which adds E, the 8-bit layer the re-issue made from W. The left eye's
  call is the same without the lens, from its own W and E. scope_zoom_test pins the order (the lens before `compositeInner`, in the one call).
- **What governs it: the weapons half, with the scope zoom key arming it.** The weapons layer has no key of its own. `uiLayerWeaponsLiveFor(temporal, jitterAsShipped, stoodDown)`: a temporal mode on (the Performance
  page's Anti-aliasing, not off), the layer and the weapons path not stood down, VR (the flat profile has its own adapter). `uiLayerFamilyEnabled` maps the mark to that half and the scope zoom key arms it
  (`scopeZoomMarkArmed`, in the draw hook's facts). Not fix.ui_quality's half: the mark is no longer an interface draw, and with UI quality off A2 left it in the scene, magnified with the world, for a reason that is
  gone. Not the scope zoom key alone: the key says the lens is wanted, not that W and its tonemap states exist, and a weapons layer that stood down must give the mark back to the scene. The lens needs a temporal mode
  too (its picture is the upscaler's), so the mark and the lens are live together. edvr.ini's text for the key said UI quality and now says Anti-aliasing.
- **The blend rewrite stays, scoped to the mark.** `uiLayerBlendShape` refuses destination-alpha factors and W's blends come through the same conversion; W's alpha is the clear's 1 and additive draws leave it, so
  ONE, DEST_ALPHA read as ONE, ONE (what the alpha-less game target gave) is still the right input. `destAlphaOne = markFamily && !uiLayerFormatHasAlpha(fmt)`, at the decision and at the issue.
- **The weapons fire is not touched by the mark.** The layer counts its draws by kind (`wFires`, `wMarks`) and `weaponsLayerLoss` (weapons_fire.h) attributes a dropped layer by kind. Each kind has its own loss count,
  streak and stand-down: the fire's `weaponsStandDown` (which also returns the mark to the scene, the layer being gone), and the mark's `markStandDown` (the mark alone, after 30 frames lost in a row, with a line of
  its own). A frame that held a mark alone is not a weapons content frame, a weapons composite or a weapons loss: the 30 s weapons line counts weapons fire exactly as before (the content frame at the first fire draw,
  the matched and plain kinds for frames with fire, the lost lines for fire; the mark's first lost frame prints one labelled line). sharp_weapons_test runs the bookkeeping frame by frame (marks lost never stand the fire
  down, fire lost never stands the mark down, carried marks between lost fire frames do not reset the fire's streak) and pins the source of each.
- **S0 census.** `censusNow` asks `lensOpticalSight` (scope_zoom_s1.h: the table's entry with M >= 1.05 at the reference FOV, which native_frame_test shows is the gate's `kNotOptical` read the other way) before it
  configures or arms the pixel probe. A press of any other sight writes `scope lens census (S0): run skipped (press N is not an optical sight: ...)` and probes nothing. `--health` reads that as no failure (IDLE when it
  is all there is) and keeps NEVER APPLIED for an optical scoped press the census never reached.
- **Instruments.** The S1 press line says `gunsight marks taken: L, R` (the draws W took; it said `redirected`), keeps `armed frames, no mark draw` and the depth paths, and ends the mark's part with
  `composites matched/plain/none`: the eye-frames, both eyes, whose W held a mark, counted at the door by how the composite served them (`MarkFrame` in `uiLayerComposite`, whichever way it returns): the tonemap
  difference, the plain add (the re-issue fell back), or none (no composite reached the picture). `--health` makes a due press with marks taken a PARTIAL when matched is under 80 percent of the eye-frames counted,
  naming plain (brighter than stock) and none (no mark anywhere), and when marks were taken and no eye-frame was counted at all (the door's count never reached). A2's press line no longer parses (UNPARSED), so an old
  build cannot pass as this one. The worst-case line (every counter six digits) is 1166 bytes; the buffer is 1168 (it was 1160: the log keeps 1167 and the line needed 47 bytes), and the bound with the log's stamp
  and marker inside 1200 holds.
- **If it did not work, the log says which way.** The mark never reaches W (the shape, arming or atlas test fails; the weapons half or the mark's route is down; the layer declines it): `taken L 0, R 0` beside armed
  frames above 0, composites 0/0/0, `--health` PARTIAL "no mark draw was taken into the weapons layer", and the family census names the decision (a stood-down half is `the key that governs this family is off`). Taken and
  never composited: `none` high, PARTIAL "no composite carried them", the mark in neither the scene nor the picture, and after 30 frames in a row the mark's stand-down line puts it back in the scene. Taken and
  composited plain: `plain` high, "brighter than stock". Taken and the composite note never run: composites 0/0/0 beside marks taken, PARTIAL with its own reason.
- **Departures.** (1) The mark's own stand-down, `g_markStoodDown`: not in the brief; it is what lets a mark that never comes back return to the scene without standing the weapons fire down. (2) The press line's
  buffer, 1160 to 1168. (3) A press of another sight writes a census skip line (the brief: probes nothing); the line is what lets `--health` tell it from a settled press the census never reached.
- **Not verified, for the flight.** (1) The brightness against stock, by the card above (with the key off the chevron is the stock one). (2) That the matched composite serves the mark: `composites matched` near
  L + R taken, in both eyes. (3) The mark's position and sharpness on c (the jitter cancel is still inferred). (4) The cost: in a scoped ADS press the weapons composite (the source pass at the layer's size, two
  tonemap draws, the clear) now runs every frame in both eyes, where it ran only while weapons fired, and W (R16G16B16A16_FLOAT at the layer's size; `sharp weapons: ... weapons layer created` prints the MB) is made at
  the first scoped ADS; neither is measured. (5) The mark no longer blooms: it is in neither the scene's bloom input nor the HUD layer's feed (W2's accepted limit, now the mark's too). (6) The left eye aiming
  (`fix.native_on_foot_ads_sight_eye = left`) still gets no lens and no mark take.
- **Flight card (A3), additions.** Read `--expect-build c738e367 --health` first (S1 OK). Then the press lines: `composites matched/plain/none` against `gunsight marks taken` (matched near L + R), the `scope mark`
  family lines and the `sharp weapons: ... weapons layer created` lines; and the S0 census lines, which should show runs for the Executioner, the Oppressor and the Aphelion and a `not an optical sight` skip for the
  P-15 and the Karma weapons.

**As built (A3 review fixes)** (2026-10-29, on A3 `655993ca` merged with main `dcfd0608`; the full local build and all its gates green, nothing flown; main's merge compiled and passed untouched). A review of A3
found ten things. Each, in one line:
- **1. A mark-side failure no longer stands the weapons fire down.** `weaponsTakeReady(ctx, eye, forMark)`: when the mark asks and W, E or the tonemap states cannot be made, `markStandDown` runs (the mark alone, its line
  naming the step) and `weaponsStandDown` does not; the weapons fire's own failures stand its half down exactly as before, and it tries W for itself at its next draw.
- **2. The mark has composite lines of its own.** Gated on the layer's mark count (`e.wMarks`), not the fire's: the first eye-frame carried by the tonemap difference, and, at most once a press, why a frame went out plain
  (the source pass's shaders, the game's HDR input, the deferred context, the scratch, or the pass failing: read from the same checks `matchedReady` makes, in the same order). The weapons fire's lines are untouched.
- **3. The mark's stand-down lasts the press, not the session.** `MarkStandDown` (weapons_fire.h) holds the S1 press number it was found in (`scopeZoomPressId`); the next press of the sights tries the route again, with
  both eyes' streaks at 0. It stays global across the eyes for the rest of its press. `g_markStoodDown` became `g_markDown`.
- **4. Each stand-down is logged and counted.** The line names the press, the eye and the streak (0 for a creation failure) and the cause; the S1 press line ends the mark's part with `mark stand-downs N`; the mark's
  lost-layer line is one a press (it was one a session); `--health` names the stand-downs and makes the press a PARTIAL.
- **5. The S0 census pin.** It reads the guard's block whole and requires the skip line, the once-per-sight test and then the `return`, with neither the configure nor the arm inside it. Controls: the guard that logs and falls
  through, one that arms before it returns, a skip line on every press, no table, no skip line.
- **6. The skip line is written once for each (item, scope) a session** (`CensusSkipNoted`, scope_zoom_s0.h: 16 pairs, then every new sight is still told, to the press line cap). `--health` matches a geometry line to its
  skip line by item and scope and says `N sight(s) ... (first press P)`. A `--since` window that starts after a sight's first press has its geometry lines and not the skip line, and reads as a census that never ran.
- **7. The press line's worst case is the longest item the held-item buffer can hold.** `kLensItemChars` is 63 and the formatter cuts the item there, so the bound does not rest on the caller. With 63 characters, every
  counter six digits and the new count, the line needed about 1216 bytes against the log's 1167. Eight labels are shorter (`not optical`, `jitter none`, `other frame`, `FOV from bare copy`, `c to far point`,
  `picture seq`, `mark depth`, `read on`): the worst case is now 1160 bytes in the 1168-byte buffer, inside the log's 1200 with its stamp and marker. `edvr_log.py` reads the new labels; a press line of an earlier build is
  UNPARSED, as A2's and A3's first were.
- **8. `flatUiFamilyLive` asks the one list** (`uiLayerFamilyUsesWeaponsLayer`), so the mark is the weapons half's there too. Flat still never takes it: it is not in `flatUiLayerTakesFamily` and the flat tick publishes no
  lens. Rig: the three halves in every combination, and the take list.
- **9. The weapons layer line names what made it** (`for the weapons fire`, `for the scope's gunsight mark`) and says both draw into it.
- **10. `--health` names every reason, and the door pin's window is tight.** A press that fails the takes, the composite and the stand-down shows all three, joined by `; also`. scope_zoom_test reads 8300 bytes of
  `uiLayerComposite` (8022, the function) where it read 12000.
- **Not verified.** Nothing is flown. A creation failure at the mark's take is not driven by any rig: its source is pinned (with controls) and the stand-down's lapse and streak bookkeeping are run frame by frame against
  the production helpers (`MarkStandDown`, `weaponsLayerLoss`), not through the draw hook. The mark's engaged and plain lines, the stand-down line and the `mark stand-downs` count have never appeared in a log. The
  worst case leaves 7 bytes under the log's 1167: a new field in the press line needs a label cut again.
