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
- **Next flight: none for the table.** Separate arc: a flat stutter on the scoped Executioner's ADS (2026-10-29), in its own doc,
  `docs\investigation-flat-scoped-ads-stutter-2026-10-29.md`, on its own branch.
- **Decisions waiting on the maintainer:** A first, B3 as phase 2 for the scope (recommended, section 2); scopes only or every sight
  (section 4); the reference FOV M is taken from (section 4).
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
- There is no strength slider: M is the game's value. S0 itself is measure only, with no key, as F141 was.

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
