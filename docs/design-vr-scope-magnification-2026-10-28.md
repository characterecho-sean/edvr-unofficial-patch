# VR scope magnification: the game's zoom inside the scope (design, 2026-10-28)

Parent arc: `docs\design-native-onfoot-vr-2026-10-13.md` (F141 to F153). Native on-foot VR puts the ADS eye on the game's sight line
E -> S at one depth per weapon (F150, F152). A scope does not zoom in VR, because a headset cannot change its FOV. This design shows the
game's own zoom as a magnified picture inside the scope's lens. The rest of the view stays at true scale.

## Status

- **State: DESIGN ONLY.** No code, no key, nothing built or flown. Branch `claude/vr-scope-zoom-design-6ef950`.
- **Recommendation: option A** (section 2): after DLSS and RCAS, copy the aiming eye's picture inside the lens, magnified by the game's
  zoom M about the sight point (the shield ripple's route). Under 0.1 ms, one eye, scoped ADS only; no scene pass, no motion vectors, no
  DLSS change; F106 carries it to the left eye. The price is detail: 1/M of the output's density.
- **Phase 2, only if A is too soft:** B3, the left eye rendered as the zoomed view (a pass F106 already covers). Risks in section 2.
- **The zoom's source is not found yet** (static RE, build 334925, section 1):
  - ruled out: the bend's per-frame pair (weapon data +0x74/+0x78 to `[act+0xB0]->vt+0x18`) as the zoom, because its receiver is
    HumanoidAimAssistComponent (setter 0x1A8C350): aim-assist tuning, not the "zoom/FOV floats" the analysis note named.
  - ruled out: EDVR's F8 census FOV (`RC+0x16C`) as a VR zoom field, because it read -1.000 through every ADS press of
    `edvr_gfx_20261028_060723`, the scoped plasma sniper included.
  - Left: the camera FOV lerp, the readout's camera FOVs (+0x16C, +0x17C), the stat `ScopeMagnification`, the flat projection rows.
- **Open questions** (each has a log line in the first flight, section 6):
  Q1 the zoom per (item, scope); Q2 which field carries it per frame (flat, VR); Q3 absolute FOV or relative to the player's FOV option;
  Q4 what is drawn in the lens (see-through glass, opaque screen, reticle); Q5 the lens's footprint and the sight point's offset in it;
  Q6 what must stay unmagnified over it (readout, AR gunsight, HUD).
- **ruled out (cost or brief):** a third camera pass (C), magnifying in DLSS's input (E), a whole-view FOV change (D). Section 2.
- **Analysis hand-over FILED** (analysis `1547986`): `analysis\decomp\verify_20261028_ads_zoom_receiver.txt` (the receiver, the correction to
  `analysis\decomp\verify_20261027_ads_sight_line.txt` section 1, the candidates).
- **Next flight: S0**, measure only, no key. One flat session gives Q1 to Q3; one VR session gives Q2 (VR), Q4 to Q6. Section 6.
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

## 5. Config surface

- `fix.native_on_foot_ads_scope_zoom` = on | off, live, menu "ADS: scope zoom" under "Aiming down sights". It names what the player gets.
  It is a user key, not a test key: magnified motion under "ADS: aim with head" is a comfort choice. Default on once flown OK.
- There is no strength slider: M is the game's value. S0 itself is measure only, with no key, as F141 was.

## 6. The first flight: S0 (measure only, no rendering code)

**Offline first (no flight):** read the scoped-sniper eye dumps `173533772002`, `193900687001` and `193909388002`. The frames looked at here
(`C00`) are hip views. If any frame is in scoped ADS, it answers part of Q4 before the build.

**S0 lines** (each with a `--health` rule: OK when the line holds a scoped press; NEVER APPLIED when scoped ADS frames ran with no line):
1. `scope zoom (S0) flat` (Q1, Q2, Q3), once per press: item, scope, the FOV option; the scene projection's P00 and P11 (the rows the
   temporal pass already decodes) at the hip (median of the last 0.5 s) and in ADS (settled, w at 1.0 for 0.6 s); M; the ramp time; Z1's
   +0x16C/+0x17C at both; the game's E depth. w, E and the readout's AR come from F141's bend hook and the F85b getter, and the scope from the
   journal loadout reader; all three run only in native on-foot VR today (F141, F147's m4), so S0 runs them in flat for this line.
   **Reading:** P11's ratio is the zoom whatever field carries it; a Z1 value that moves with P11 is the per-frame field; M changing with
   the FOV option means absolute FOVs.
2. `scope zoom (S0) vr` (Q2 in VR), once per press: Z1's two FOVs, the census FOV and w per (item, scope). **Reading:** a value that narrows
   with w in scoped ADS is read per frame; constant or -1 means the flat table.
3. `scope lens census (S0)` (Q4), each scoped ADS second, aiming eye: the draws whose footprint covers a ring of 8 points at 0.25, 0.5 and 1 x
   the expected lens radius about c, with VS/PS hash, blend (opaque / alpha / additive), stencil 0x10 or not, depth against the sight
   point's, and the coverage fraction. **Reading:** a blended draw over world depth is a glass and A applies; an opaque draw nearer than S is
   a screen (section 3's opaque case); a small draw at c only is a reticle.
4. `scope lens geometry (S0)` (Q5): c in pixels; the lens bounding box from the named draw and from the stencil hole; the radius in pixels
   and degrees; |c - centre| / r; EDVR's eye depth against the game's scoped E.
5. `scope lens overlap (S0)` (Q6): for each readout part, the AR gunsight part and HUD draw near c, its footprint's overlap with the lens and
   whether it is drawn before or after the door.

**Flight card.**
- Flat (Epic, or the unified install's flat mode): every weapon, hip to ADS three times, scoped and unscoped copies (the Executioner both,
  and any other scoped weapon owned). Change the FOV option once and repeat the Executioner.
- VR (Frontier): the scoped Executioner in ADS, 10 s still and 10 s turning the head; the unscoped copy; one eye dump in scoped ADS.
- Read `--health` first, then the five lines.

**After S0:** see-through glass and M under about 2.5 for every zooming weapon: build A behind the key. An opaque screen: A with that draw
moved after the door, or B3. Zoom above 2.5: A's detail is likely too low, so put B3 to the maintainer. A per-frame VR field found: read it;
otherwise bake the flat table.
