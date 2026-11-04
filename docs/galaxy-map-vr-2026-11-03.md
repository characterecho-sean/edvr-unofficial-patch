# VR Galaxy Map: motion, icons and the cursor

## Status

State (2026-11-04): all four reported symptoms have a named cause; fixes 1, 2 and 3 are BUILT on branch `claude/galaxy-map-motion-cursor-e864cb`, full build green, NOT FLOWN, not merged.

Evidence: flight 2, v0.22.0-rc.6-21-g7c1544a0, Frontier, Galaxy Map aboard, log `edvr_gfx_20261104_060905`, dumps 061416462001, 061443363002, 061450674003 (head yaw -28, -3, +17 degrees, map still). Environment: native OpenXR runtime, 2016x2017 in, 4032x4034 out, DLSS performance.

| Symptom | Cause | Standing | Fix |
|---|---|---|---|
| Yawing rotates icons | EDVR's own roll-free basis (`fix.particle_billboard`) uses galactic up on a map camera pitched 45 degrees | MEASURED, within 0.4 degrees in all three dumps | Take three shaders out of the roll-free table |
| Map movement smears lines and text | 360 to 450 overlay draws per eye are in the scene and reach DLSS; only flat has them in the UI layer. The stillness bound also keeps history on slow moves | MEASURED | VR map families in the UI layer; reset on the map camera's exact change |
| Cursor does not select stars | Arrow is at about 1 m on the panel shell; the hover uses a different point, about 1.83 times the arrow's angle | Arrow MEASURED; hover gain INFERRED, fits better than gain 1 | Hover from the arrow's direction, far away; arrow and panels at star depth |
| Moving the headset changes the system | The hover point and the arrow are near, the stars are at infinity. The head's move between dumps carried the eyes 6.7 cm (about 0.065 tan at 1 m); labelled stars near the pointer are 0.02 to 0.05 tan apart | Geometry MEASURED; that this is the mechanism is INFERRED (K's camera OPEN) | Same fix; it does not depend on K's camera |

Decided (Sean, 2026-11-04): panels at star depth.

Open, the flight's log answers the first three: who writes K's matrices and which camera they are (the `map pointer check` lines print them); whether a click re-tests the star (candidate: vis+0x2C0 copied from +0x2B0; the fix logs any other caller of K's projection); whether `SystemMapView` is the System Map's stage name (the `map camera:` lines name every stage 0). Not answered by it: the hover pointer block's class ([vis+0x110]), now only of interest for the gain; System Map and Orrery hover (not read, so fix 3 is Galaxy Map only).

Temporary to remove when this closes: the `map pointer check` lines and their second K call. The F111 map probe and the stillness bounds are gone. No new keys.

Next: the flight (card below). RE notes wait for the `elite-analysis intake` session: analysis-inbox folders 20261103-202226-galaxy-map-pointer, 20261104-gm-hover, 20261104-gm-camera.

Ruled out:
- ruled out: the left eye missing panels and the arrow in play, because it is the dump's first frame only. At each run's start the log says `eye unknown` for the interface and cursor draws and counts one swap (5400 of 5400 eyes matched in the window before); frames 1 to 15 have the UI in both eyes. The overview images are taken from frame 0, so do not read a left-eye overview for the UI. Eye dumps lose frame 0's left UI: a small bug in the dump's arming, not looked into.
- ruled out: the arrow being head-locked, because its record's direction stays within 0.01 degrees while the head yaws 8.7 degrees in dump 061416462001.
- ruled out: the rho lever (M1) on the map, because the panel's pick owner is the upright 0x4526E40.
- ruled out: no jitter and no history for the whole map (fix 1), because every frame is then a single un-antialiased upscale and the tremor makes thin text shimmer.
- ruled out: the labels' fixed quaternion as the yaw symptom, because those quads are world-fixed and do not turn; the triangles do.
- ruled out: a symmetric-vs-asymmetric frustum mismatch alone (it gives a constant 0.19 tan in x only). Head-pose latency (head rows are about identity frame to frame).

## What the dumps show

**Two frames, one head-free pose.** Each frame has four cameras (refresh hook, kind 5): left and right in the map frame, left and right in a UI frame. The UI frame's axes are the map camera's base rotation, its origin (0,0,0). The head enters both as rotation and as a real-scale eye offset (eye separation about 0.056 in each frame's units).

The camera source's stage 0, named `GalaxyMapView`, holds the head-free pose: rows (1,0,0), (0,0.7071,0.7071), (0,-0.7071,0.7071), position (49996.656, 41092.668, 23990.863), identical to the last digit in all three dumps while the head swings 45 degrees (`pool\camera_<stamp>.json`, read by flat_stationary_camera_capture.cpp's hook on 0x2874290). This is the map's exact motion signal and the base for every direction below.

**The frame's draws, per eye, in order** (census at 06:14:50):

| Order | Draws | What | Today |
|---|---|---|---|
| 1 | 9A94, 78411B x2, 88DCF1 | sky, Milky Way | scene |
| 2 | E9A2F9 (n=576), 41C518 (52472 instances), 7D26BC (15291), 39D858 (384) | stars, dust | scene |
| 3 | CFA918, B60790, DEF19B | bloom | scene |
| 4 | C31238 ps 964221 (n=6 167 to 254, n=12 102 to 118), 12382D ps 855C46 (70 to 76), 94EA99 ps 65DAC5 (348 instances), 3028E7 ps 6C28D5 (315 instances) | icons and triangles, labels and grid, rings, stems | scene, reaches DLSS |
| 5 | 6F4B7B ps 5EC486 (scene at t0, bloom at t1), 20F383 ps DED879 | tonemap, copy to RGBA8 | game |
| 6 | C4B4B3, B10B03, 5DAD41, 4EF6DD | panels, popups, arrow | UI layer already |

Everything the map moves that has an edge is in row 4, after the bloom.

**Three classes of quad** (pool records joined to draws through the instance buffer, `pool\pool_*`, `inst_*`):
- Labels and n=6 icons: a quaternion the CPU writes, equal to the base pose with a 180 degree flip, the same in all three dumps. Fitted from text baselines and icon edges their right axis is world (1.00, 0.00, -0.01) and up (0.00, 0.70, 0.71) in every dump. They are fixed in the world and do not turn with the head. Leave them.
- n=12 triangles: identity quaternion, spanned on cb1[277] and cb1[278]. These turn.
- On-plane labels ("10 : 88 : -100") and discs: quaternion flat on the galactic plane. Correct.

**The arrow.** Pool record with scale about -0.43 (RE: the 0.4 size). P = (-0.336, 0.338, 0.916), |P| = 1.032, in the UI frame. Direction fixed to 0.01 degrees while the head moves, so C2's rotation is head-free on the map (on foot the 2026-10-28 RE note measured it head-tracked). That the draw reads this record also answers the 11-03 question: it reads the scene object (RE block b), whose tan is P.x/P.z, and not the block rescaled by the two projections.

| Dump | Head yaw, pitch | Arrow tan in the UI frame | Selected-star popup tan |
|---|---|---|---|
| 061416462001 | -28.2, +4.1 | (-0.367, +0.368) | (-0.641, +0.780) |
| 061443363002 | -2.6, +8.0 | (-0.123, +0.256) | (-0.366, +0.416) |
| 061450674003 | +17.4, -0.6 | (+0.516, -0.032) | none |

Left and right eye together put the arrow at 0.049 tan of disparity (dump 061443363002, frame 8), the panel's text at 0.053, stars at 0.004. The eye origins explain it: 0.05 / 1.03 = 0.049.

**The hover** (RE, READ unless marked): GalaxyMapVisualisation 0x3B91D80 and 0x3ADE6F0 compare viewport x, y of the pointer and each star, both through K = 0x28948F0 over 0x2894A50, which treats its input as a world POSITION. The pointer block is INFERRED to be the cursor's second matrix MC+0x60, `sprite * C2.proj * inverse(C1.proj) * C1.pose`: the sprite's x and y scaled by tan(C1)/tan(C2) = 1.83 and placed about 1 unit from the camera.

Test against the 382 star positions in the pool: at gain 1.83 the popup's star is the 12th and 19th nearest to the pointer, at gain 1 the 170th and 87th. Better, not exact (the game weights by star size and holds a hover for 0.3 s). The fix replaces this point, so it does not rest on the fit. Dump 3 shows the symptom: the arrow is on Orobriges, gain 1.83 puts the pointer at tan 0.94, and nothing is selected.

## Fix 1: icons (EDVR's bug, the smallest change)

`fix.particle_billboard`'s roll-free rebind builds right = cross((0,1,0), row 279). In cockpit space that is horizontal. On the map (0,1,0) is galactic up and the camera looks 45 degrees down onto the plane, so the basis rolls against the eye with every yaw.

| Dump | EDVR's basis predicts | Clean magenta triangles measure |
|---|---|---|
| 061416462001 | +20.7 degrees | +20.3 (3 triangles) |
| 061443363002 | +2.4 | +2.8 (5) |
| 061450674003 | -14.3 | -14.6 (6) |

The match also shows row 279 is the eye's forward, so the game's own rows 277 and 278 are the eye's axes and stock quads stay upright in the eye.

Change: remove 12382D2EA45E9632, 94EA991C3A03DBD9 and C31238331D8AC3D4 from the roll-free table (particle_fix.cpp:234, particle_fix.h:62, the static checks' count). They appear in no other scene: 31 of 639 roll-free windows across the last 25 Frontier logs name them, all at map rates (over 100 draws a frame). This also drops the 5 block copies a frame the map pays today. F1502353BE051A2D is in no recent window; leave it.

Tests: the rollfree rig's table, plus a basis check that names the map case so the table cannot regrow.

## Fix 2: lines and text in the UI layer

Take row 4 of the table into the HDR layer while a map is open, and let DLSS keep only what has no edges to smear.

- **Families.** flat_ui_layer_math.h names kMapCanvas and kMapSprite (including ps 964221955A9E77E7) and the path exists: begin, issue, tone re-issue, composite. VR gets the same pairs behind the same gate, plus a new family for the rings (94EA99 ps 65DAC5) and stems (3028E7 ps 6C28D5), which flat leaves in its scene. Flat's rule does not change.
- **Tone.** VR admits a tone by the identity of the HDR target it samples (uiLayerCrispNoteEyeDraw), so no table entry is needed: the census shows the tone 6F4B7B60C6C15BBF with 5EC48683F6C86A42 reads at t0 the texture the overlays draw into. Its pixel shader combines scene and bloom as `cb2[1].w >= 0 ? lerp(scene, bloom, w) : scene + bloom`, and the re-issue zeroes t1, so with w at or above zero the layer would be dimmed by 1 - w. w is not known, so the re-issue for this shader binds a copy of b2 with cb2[1].w = -1, the additive branch, and a zero bloom leaves T(L) exactly.
- **Order.** Row 4 is contiguous and after the bloom, and the panels land on top after the re-issue, so the picture's order holds.
- **History.** Stars, dust and sky stay in DLSS with jitter and history at rest, which is what fix 2 of the gate gave them. A reset is decided by stage 0's pose changing: any change between frames, no threshold. This replaces `temporalMapCameraStill` and its 0.15 degree and 0.05 bounds. Those are the slow-move smear: position noise at rest peaks at 0.016, the bound was three times that, and the flight's kept frames reached 0.049.
- **Gate.** The same hook gives the stage name, so `GalaxyMapView` opens the Galaxy Map's gate on its first frame instead of up to 0.5 s later. `SystemMapView` sits beside it in the exe's rdata (0x518E278) and publishes the same way, unproven as the System Map's stage until the log names it; GuiFocus 6 to 8 still opens the gate on its own, with no history since nothing publishes. RE also found IsInGalaxyMap (0x3B36E70, 0x3B85110) if the hook proves unsuitable. The hook on 0x2874290 existed for the dump's capture; in VR it now stays installed, reads only stage 0, and publishes the pose across threads (map_camera_core.h). The look after a change resets as well, because a frame's draws can be one publish behind.
- **If the tone is ever missed.** A map frame whose tone never published has no icons, since they left the scene. After 30 such frames in a row the whole HDR path stands down with a back-off, cockpit HUD included. The census says this will not happen; `lost HUD layer` lines would show it.
- **Light through the tone.** Rings and stems are additive, so they go through the tone as T(L) added after T(scene), not T(scene + L), like the space dust. Icons and labels classify as over, with no blend path of their own.
- **Cost.** 360 to 450 redirected draws per eye against about 6 today, plus stencil write-backs. Map frames have room (7.4 ms wait in the flight's timing line). Read `draw hook CPU` in the flight; if it matters, bracket the contiguous run with one target switch rather than per-draw state.

This also ends the at-rest text shimmer for good: text is drawn at output resolution with no jitter, and owes DLSS nothing.

## Fix 3: one space for arrow, panels and stars

The map is at infinity; the panels and arrow sit about 1 m away in a frame only the UI uses. Make everything in the map share the stars' depth, and make the hover read the arrow.

**(a) Panels and arrow at star depth.** All four UI vertex shaders (C4B4B3, B10B03, 5DAD41, 4EF6DD) subtract cb1[275].xyz, the eye's origin in the UI frame, use it nowhere else, and their clip rows are camera-relative. While the Galaxy Map is open and (b) is armed, VS slot 1 holds a copy of the bound block with row 275's xyz zeroed (map_ui_depth.h: the partial copy route of billboard_rollfree.h, made at each such draw, with no write tee). The rest of the block keeps the head's rotation.

Each eye then sees the UI from the frame's origin. Disparity goes to zero, head translation stops mattering, and the arrow's direction is exactly the direction the pick ray used, so panel clicks land where the arrow is drawn. Only the pixel's depth changes; the pick mesh, Flash and the game's arrow stay as they are.

Cost of this choice: the panels lose their stereo depth and read at the stars' distance. The map's content is already there, so nothing needs re-converging. The alternative keeps panels at 1.2 m and moves only the arrow, which then doubles over a panel unless an over-panel test switches it. RE found no such signal: controller +0x100 is mesh-level, reads the same over empty shell and a button, and 0x455A720 only pushes into the movie. That leaves Scaleform's cursor event (untraced) or reading the layer's alpha back under the arrow.

**(b) Hover from the arrow.** Hook K's projection (0x28948F0, prologue `48 89 5C 24 08`, 5 bytes) and act only on return address 0x3B91EE9, the pointer's call in 0x3B91D80. Replace the input point with `position + 100000 * (R * normalize(P))`, R and position from stage 0, P the arrow's drawn position: three floats at +0x40 of the scene object [MC+0x10], MC being rcx at the cursor's update 0x474A740, recorded by a stub of generated code that touches no argument register. The drawn position rather than the controller's +0x140, because that is what the eye sees: with row 275 zeroed the arrow appears exactly along it. The dumps check the convention: dump 3's arrow tan (0.516, -0.032) lands 0.015 from a star record.

At that distance K's result depends only on direction, so the hovered star is the one behind the arrow whatever K's camera is, and a far point removes the head's parallax. Star calls (0x3ADE6F0) pass untouched. Hook chosen over a patch inside 0x3B91D80 (0x3B91EDF) because it leaves the function's bytes alone and returns the game's own viewport units. K's relay is gated, so K pays nothing on any other screen.

A self-check runs in the hook for the first ten seconds of each opening, once a second. It prints the game's point in the map's axes, P, the ratios a/Px, b/Py and c/Pz (which give the gain and the distance with no model), the viewport point from each input, K's half extents, and W's view and projection rows. That settles the pointer block's reading, the 1.83 and K's camera with no extra flight. Any other return address reaching K with a point within 10 units of the camera is logged once with its RVA: that is how a second test at click time shows itself.

**What each symptom becomes.** The arrow stays on a star whatever the head does; the star it covers is the one selected; moving the headset changes neither.

**Known limits.**
- Reach: the arrow's range is the UI camera's, about tan 0.546 (28.6 degrees) each side of the map's forward, where the hover's was about 45. Stars past it need the map turned. Dump 3's arrow is at NDC 0.94 on Orobriges, so this will be felt. Scaling the mouse would need a depth or mode switch for the panels, so it is not in this design.
- K returns false for a direction behind eye 0, so looking more than 90 degrees from the arrow clears the hover.
- System Map and Orrery get fixes 1 and 2, not 3, until their hover is read (CursorCircle, OrreryCursor).
- On foot: the VR map has no GuiFocus; stage 0's name may open the gate there, unproven.

## Flight card (one flight, VR, cockpit, Galaxy Map open)

Right build first: `python tools\edvr_log.py --target <odyssey path> --expect-build HEAD --version`.

1. Rest, then yaw the head side to side with the mouse still. Expect: triangles stay upright; the arrow stays on its star; the same system stays selected. Insert dump.
2. Pan, orbit and zoom, fast then slowly to a stop. Expect: labels, icons, rings and stems crisp with no trail. Stars may soften while moving.
3. Hold the arrow on three separated stars and click each. Expect: that star is selected. Click a side tab and the search box to check panels.

Log:
- `billboard roll-free` no longer names the three shaders.
- `map camera:` says the watch is on and names each stage 0; `temporal aa map:` keeps most eye-frames at rest and resets while the map moves.
- `ui quality: layer` shows `map canvas`, `map sprite` and `map rings and stems`; `composites left in the scene` no longer lists 12382D2E; `crisp hud: the map tonemap` says the b2 copy is on; no `lost HUD layer` lines.
- `map pointer: armed`, then the 30 s `map pointer:` line: pointer calls replaced against passed through, and draws bound at star depth. If these are missing, the code never ran.
- `draw hook CPU` for the cost.

If the arrow lands on a star but another is selected, read the `map pointer check` lines first, then the other-callers line.

## Journal

### 2026-11-04, the build

Sean: "Let's build it", with panels at star depth. Changes against the first draft of this design, each found reading the code before building: the tone needs no table (VR admits by identity) but does need the bloom blend forced additive; P comes from the drawn scene object, not the controller; `SystemMapView` found in the exe. K's stores are `movups`, checked by disassembly, so the self-check's scratch output needs no alignment.

### 2026-11-04, flight 2 (7c1544a0), the reports this design answers

"The mouse cursor is not functional for selecting stars"; "yawing the headset rotates the icons around their axis"; "moving the headset causes different systems to be selected"; "moving the map itself causes lines and text to appear smeared or ghosted like they have not been put into the UI layer".

Log: `temporal aa map: 18636 eye-frames kept their history (at most 0.001 deg and 0.0490 off it), 1552 reset it (up to 161.846 deg and 69744.6940)`. The rotation bound was never close; the translation bound was reached. The probe opened (`map probe` line, F114b names owner 0x4526E40), but with no detail log there are no F111 cycle lines: the pool records gave the answer instead.

Rest figures from these dumps (16 frames each): rows' move 0 to 0.016, head translation 0.00002 to 0.0046. Head moving 0.45 to 1.9 degrees a frame in dump 1 and the rows follow within 0.002 degrees.

### 2026-11-04, flight 1 (948fa347), fix 2 of the gate

The no-jitter, no-history gate worked (map_frames=11164, every dump frame jitter 0 and reset) and text and icons shimmered at rest: each frame is a standalone upscale and the camera tremors about one input pixel a frame. In the history-on dump 193401951001 the pass's motion field was a clean rotation field, so history is right at rest. Built fix 2: jitter and history kept while the rows follow the head (flags bit 8, `temporalMapStillBits`). The pointer probe never opened on this flight: `temporalMapOpen(journalGuiFocus(&mapFocus), mapFocus)`, MSVC read mapFocus before the call wrote it. Fixed with a separate statement.

### 2026-11-03, flight 0 (d85658fc)

Dump 193401951001. The pass ran with jitter and history on the map; the map writes no depth (flat finding, docs\design-flat-temporal-aa-2026-09-23.md, 2026-10-10), so the pass has only the head's delta. Static RE of the cursor and the hover: analysis-inbox 20261103-202226-galaxy-map-pointer, models M1 (lever), M2 (near sprite), M3 (fov scale). The dumps of 11-04 settle it: M1 out, M2 and M3 both present (near, and scaled).
