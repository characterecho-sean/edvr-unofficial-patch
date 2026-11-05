# VR Galaxy Map: motion, icons and the cursor

## Status

State (2026-11-04): fixes 1, 2 and 3 FLOWN 09:50 (ca707034): the map is crisp and does not smear, systems select properly, the UI works. Build bf17a80a FLOWN 11:00: panels at the game's depth and the arrow's panel mask work; icons facing the eye REJECTED, they yaw with the head. Third build 6a12ef42 FLOWN 12:35: world-fixed quads and gap bridging work. Fourth build 759ec617 FLOWN 13:21: "basically works", but the map fell to 45 fps and near icons wobble. Fifth build e02c6248 FLOWN 14:13: still about 60 fps on the map, and icons and text rotate about y as the camera moves sideways. An adversarial review found both causes (see "After the fifth flight"; `reviews\galaxy-map-perf-review-2026-11-04.md`, not in git). Its course, built as the sixth build 5086b6f3, FLOWN 16:38: the map holds 88 to 90 fps, `xr_end_frame` p50 0.20 to 0.22 ms, 2 render target binds a frame. Sean chose the camera's up, so the key and the grid's-vertical option are removed. Sean: "Wobble is gone", so `mapEyeRel` and the stems' swap stay. Merged with main, which was 168 commits newer with a rewritten draw path. Main's build 5bce272d FLOWN 18:35: 90 fps on the map and 2 render target binds a frame. ON MAIN, pushed.

Evidence: flight 2, v0.22.0-rc.6-21-g7c1544a0, Frontier, Galaxy Map aboard, log `edvr_gfx_20261104_060905`, dumps 061416462001, 061443363002, 061450674003 (head yaw -28, -3, +17 degrees, map still). Environment: native OpenXR runtime, 2016x2017 in, 4032x4034 out, DLSS performance.

| Symptom | Cause | Standing | Fix |
|---|---|---|---|
| Yawing rotates icons | EDVR's own roll-free basis (`fix.particle_billboard`) uses galactic up on a map camera pitched 45 degrees | MEASURED, within 0.4 degrees in all three dumps | Take three shaders out of the roll-free table |
| Map movement smears lines and text | 360 to 450 overlay draws per eye are in the scene and reach DLSS; only flat has them in the UI layer. The stillness bound also keeps history on slow moves | MEASURED | VR map families in the UI layer; reset on the map camera's exact change |
| Cursor does not select stars | Arrow is at about 1 m on the panel shell; the hover uses a different point, about 1.83 times the arrow's angle | Arrow MEASURED; hover gain INFERRED, fits better than gain 1 | Hover from the arrow's direction, far away; arrow and panels at star depth |
| Moving the headset changes the system | The hover point and the arrow are near, the stars are at infinity. The head's move between dumps carried the eyes 6.7 cm (about 0.065 tan at 1 m); labelled stars near the pointer are 0.02 to 0.05 tan apart | Geometry MEASURED; that this is the mechanism is INFERRED (K's camera OPEN) | Same fix; it does not depend on K's camera |

Decided (Sean, 2026-11-04, after flying it): panels at the game's depth, the arrow on a panel at the panel's depth and off it at star depth. This replaces the first decision (panels at star depth), which clipped through the map in stereo.

Open: what makes VR draw the small mesh variant of the System Map's atmosphere ring and landable arc on bodies flat draws the next one on ("The System Map"); the build scales them by a measured rule meanwhile. The hover pointer block's class ([vis+0x110]) is now of no consequence. The System Map's hover is read and takes fix 3 (built 2026-11-05, not flown); the facing icons and the sprite depth stay Galaxy Map only. `SystemMapView` is the System Map's stage name (dump 050358167001).

Temporary: no keys. The self-check, the F111 map probe, the stillness bounds and `experimental.galaxy_map_icon_up` are gone. 12382D in `eye_draw_snapshot.h`'s watched list, for the System Map's markers, comes out when those sizes are read.

Next (2026-11-05, see "The System Map"; everything below went to main that day): the cursor with Anti-aliasing off and on the System Map is FLOWN OK (408e7636, Sean: "works the same"). The markers' scale is FLOWN OK (fc05ee07, Sean: "Fixed the atmospheres"). The System Map's click is FLOWN OK (22eb5c87, Sean: "Clicks work now"). Sean called the click the only difference left from flat, so the map's plane out of the AA pass is flown with nothing against it. The close view's lines in the layer FLOWN (85e2fe00, Sean: "Seems better while zooming"). The arrow from its ray, the reticule at its height and the close view's history through a move FLOWN OK (852102d1, 11:17, Sean: "Looks good"). Its log: `arrow's depth` 1.96 to 2.02 in the close view, 16.62 zoomed out, and `reticule lifted` counting. The count of kept moving frames is written when the game exits, so it was not in the log when this was recorded, and no dump was taken. Still open: the small icons' break-up. The arrow's sticky edge round the panels FLOWN OK on both maps (dabfff15, 12:23, Sean: "That fixed it"); its log shows 3173 of 3826 probes on the sticky test in one 30 s, and it went to main that day. The count of the close view's kept moving frames is still unread: the game was running when this was recorded. The sprite depth stays: Sean asked for the order and the map holds 90 fps with it. RE notes filed: `analysis\decomp\verify_20261103_galaxy_map_pointer.txt`, `verify_20261104_galaxy_map_hover.txt`, `verify_20261104_galaxy_map_camera.txt`, `verify_20261105_system_map_hover.txt`. Not flown (2026-11-05): the System Map's state, with its marker and reticule swaps, body table and close view, now follows the pointer's arming (entry "2026-11-05, the System Map stands down with its return").

Ruled out:
- ruled out: the side buttons being missing from the panel mask, because they are part of the shells' textures, at least 83% opaque against a 10% threshold (dump 141733701001), with gaps the bridging closes ("The side buttons").
- ruled out: the nearest of all known bodies as the arrow's depth, because in dump 103332590001 it read 0.78 units against 1.98 to the surface under the arrow: that body was behind the camera.
- ruled out: taking the settlement icons into the UI layer as a change to make, because both their draws are already there (88DCF1 in `map plane icons`, 12382D in `map canvas`; 34 draws a frame with the reticule).
- ruled out: a marker atlas with no mips as the small icons' break-up, because the log says 11 mip levels, anisotropic, bias 0.00.
- ruled out: jitter with no history as the surface's flicker, because a reset frame carries no jitter (`jitter0` 0 in the dump's motion.csv). The raw frames alias on their own.
- ruled out: a LOD on range holding VR at the small marker variant, because VR draws the same meshes at camera height 12 and at 80 (dumps 072747469002, 072734614001).
- ruled out: reading VR's meshes as the fifth variant from the index offset between sessions, because that offset is the pool's load order: 32 indices in one VR session against flat, 6512 in another.
- ruled out: 338A85 (BD801F) as the landable arc, because it is one draw a frame against seven arcs in both modes, and it sits in the run of the star's draws; and E79BF3 (4724A8) as the atmosphere ring, because the ring's bodies and count are 12382D's and E79BF3's shader is most opaque at a surface's centre. Both were first guesses in this doc, and neither draw's constants have been read.
- ruled out: a reader of the `SystemMapPlanet_*` set in the System Map's code, a run-time name, and a radius-to-index ladder in its node code, because a scan of the whole image and every caller of the node's radius, scale and extent slots found none (`analysis\decomp\verify_20261105_system_map_hover.txt`, section 4).
- ruled out: the atmosphere pass (9FFA5D, 8134D0) as the System Map's small blue shell, because in dump 050358167001 its camera constant cb1[3] equals the eye-relative body position over the outer radius to four digits and to 0.000 degrees, per eye, and its inner radius matches the planet's silhouette to the pixel in both eyes.
- ruled out: making the pointer stand aside when the UI layer is not live (35a97739 on `claude/rc7-review-fixes`), because the hover then goes back to the game's near point and the cursor stops selecting stars with Anti-aliasing off (Sean, 2026-11-05). The hover needs no layer, and the mask and probe now run without it.
- ruled out: the grid's vertical as the quads' up (key value `stems`), because with the camera 45 degrees down a quad at view tangents (x, y) has 1 + y times its height and shears by atan(x / (1 + y)). Flown 16:41 beside the camera's up; Sean prefers the camera's up.
- ruled out: B10B032BDFD46700 as a panel, because its n=6 records sit in the UI frame at their own depth: the hover halo round the hovered star (dump 123832862001, record 1258) and the heading text. In the mask, the halo put the arrow at panel depth whenever a star was hovered.
- ruled out: dropping the stencil write-backs as the whole frame-rate fix, because `restore()` still rebinds the game's D32S8 around every sprite issue (ui_layer.cpp:2217), so the private D16 and the game's depth-stencil still alternate 2,600 to 3,500 times a frame. The benchmark behind that fix had no restore. Flight 14:13: about 60 fps.
- ruled out: `transfer` in `native timing CPU` as a frame-rate gate, because it is a single frame's sample in each 10 s. Use `xr_end_frame` p50: 0.21 ms on the third build, 5.09 on the fifth.
- ruled out: axes from each quad's own ray, because they turn every quad as the camera moves (14 degrees per unit of sideways move at 4 units) and fan label baselines from +29 to -17 degrees across one image. No report asked a quad to face the eye; that was this doc's addition.
- withdrawn: an earlier line here ruled out the map camera's up for the sprites. Its reasoning did not fit its own report: the camera's up and the grid's up drew the same complaint under the same motion, and what they shared was the ray.
- ruled out: spanning the map's quads on the eye's axes (build bf17a80a), because that locks them to the eye. Dump 110152762001: every icon's top edge measures 0.00 degrees in the image while the head turns from yaw 19.6 to 4.7. Sean: "the icons and text yaw with my head".
- ruled out: panels at star depth, because they clip through the map's content in stereo (flight of 09:50, Sean).
- ruled out: K's camera following the head, because the self-check printed its view rows equal to the head-free map camera's, with projection 0.939 on both axes.
- ruled out: a second test of the pointer at click time, because the only other near caller of K was the stars' own call in their hover test, and selection worked.
- ruled out: a stencil from the panels' geometry, because the shell meshes (C4B4B3, 5DAD41, n=360) take their shape from a mask texture's alpha. The 8-bit layer's own alpha will not do either: the map's overlays reach it through the coverage pass.
- ruled out: the left eye missing panels and the arrow in play, because it is the dump's first frame only. At each run's start the log says `eye unknown` for the interface and cursor draws and counts one swap (5400 of 5400 eyes matched in the window before); frames 1 to 15 have the UI in both eyes. The overview images are taken from frame 0, so do not read a left-eye overview for the UI. Eye dumps lose frame 0's left UI: a small bug in the dump's arming, not looked into.
- ruled out: the arrow being head-locked, because its record's direction stays within 0.01 degrees while the head yaws 8.7 degrees in dump 061416462001.
- ruled out: the rho lever (M1) on the map, because the panel's pick owner is the upright one (`docs\build-334925.md`, "VR Galaxy Map").
- ruled out: no jitter and no history for the whole map (fix 1), because every frame is then a single un-antialiased upscale and the tremor makes thin text shimmer.
- ruled out: the labels' fixed quaternion as the yaw symptom, because those quads are world-fixed and do not turn; the triangles do.
- ruled out: a symmetric-vs-asymmetric frustum mismatch alone (it gives a constant 0.19 tan in x only). Head-pose latency (head rows are about identity frame to frame).

## What the dumps show

**Two frames, one head-free pose.** Each frame has four cameras (refresh hook, kind 5): left and right in the map frame, left and right in a UI frame. The UI frame's axes are the map camera's base rotation, its origin (0,0,0). The head enters both as rotation and as a real-scale eye offset (eye separation about 0.056 in each frame's units).

The camera source's stage 0, named `GalaxyMapView`, holds the head-free pose: rows (1,0,0), (0,0.7071,0.7071), (0,-0.7071,0.7071), position (49996.656, 41092.668, 23990.863), identical to the last digit in all three dumps while the head swings 45 degrees (`pool\camera_<stamp>.json`, read by flat_stationary_camera_capture.cpp's renderer hook). This is the map's exact motion signal and the base for every direction below.

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

**The hover** (RE, READ unless marked): GalaxyMapVisualisation's pointer hover and its stars' hover test compare viewport x, y of the pointer and each star, both through K, the map's projection (addresses in `docs\build-334925.md`, "VR Galaxy Map"), which treats its input as a world POSITION. The pointer block is INFERRED to be the cursor's second matrix MC+0x60, `sprite * C2.proj * inverse(C1.proj) * C1.pose`: the sprite's x and y scaled by tan(C1)/tan(C2) = 1.83 and placed about 1 unit from the camera.

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
- **Gate.** The same hook gives the stage name, so `GalaxyMapView` opens the Galaxy Map's gate on its first frame instead of up to 0.5 s later. `SystemMapView` sits beside it in the exe's rdata and publishes the same way, unproven as the System Map's stage until the log names it; GuiFocus 6 to 8 still opens the gate on its own, with no history since nothing publishes. RE also found two IsInGalaxyMap functions if the hook proves unsuitable. The renderer hook existed for the dump's capture; in VR it now stays installed, reads only stage 0, and publishes the pose across threads (map_camera_core.h). The look after a change resets as well, because a frame's draws can be one publish behind.
- **If the tone is ever missed.** A map frame whose tone never published has no icons, since they left the scene. After 30 such frames in a row the whole HDR path stands down with a back-off, cockpit HUD included. The census says this will not happen; `lost HUD layer` lines would show it.
- **Light through the tone.** Rings and stems are additive, so they go through the tone as T(L) added after T(scene), not T(scene + L), like the space dust. Icons and labels classify as over, with no blend path of their own.
- **Cost.** 360 to 450 redirected draws per eye against about 6 today, plus stencil write-backs. Map frames have room (7.4 ms wait in the flight's timing line). Read `draw hook CPU` in the flight; if it matters, bracket the contiguous run with one target switch rather than per-draw state.

This also ends the at-rest text shimmer for good: text is drawn at output resolution with no jitter, and owes DLSS nothing.

## Fix 3: one space for arrow, panels and stars

The map is at infinity; the panels and arrow sit about 1 m away in a frame only the UI uses. Make everything in the map share the stars' depth, and make the hover read the arrow.

**(a) Panels and arrow at star depth.** All four UI vertex shaders (C4B4B3, B10B03, 5DAD41, 4EF6DD) subtract cb1[275].xyz, the eye's origin in the UI frame, use it nowhere else, and their clip rows are camera-relative. While the Galaxy Map is open and (b) is armed, VS slot 1 holds a copy of the bound block with row 275's xyz zeroed (map_ui_depth.h: the partial copy route of billboard_rollfree.h, made at each such draw, with no write tee). The rest of the block keeps the head's rotation.

Each eye then sees the UI from the frame's origin. Disparity goes to zero, head translation stops mattering, and the arrow's direction is exactly the direction the pick ray used, so panel clicks land where the arrow is drawn. Only the pixel's depth changes; the pick mesh, Flash and the game's arrow stay as they are.

Cost of this choice: the panels lose their stereo depth and read at the stars' distance. The map's content is already there, so nothing needs re-converging. The alternative keeps panels at 1.2 m and moves only the arrow, which then doubles over a panel unless an over-panel test switches it. RE found no such signal: controller +0x100 is mesh-level, reads the same over empty shell and a button, and the function that forwards its input only pushes into the movie. That leaves Scaleform's cursor event (untraced) or reading the layer's alpha back under the arrow.

**(b) Hover from the arrow.** Hook K's projection (prologue `48 89 5C 24 08`, 5 bytes) and act only on one return address, the pointer's call in the pointer hover. Replace the input point with `position + 100000 * (R * normalize(P))`, R and position from stage 0, P the arrow's drawn position: three floats at +0x40 of the scene object [MC+0x10], MC being rcx at the cursor's update, recorded by a stub of generated code that touches no argument register. The drawn position rather than the controller's +0x140, because that is what the eye sees: with row 275 zeroed the arrow appears exactly along it. The dumps check the convention: dump 3's arrow tan (0.516, -0.032) lands 0.015 from a star record.

At that distance K's result depends only on direction, so the hovered star is the one behind the arrow whatever K's camera is, and a far point removes the head's parallax. Star calls pass untouched. Hook chosen over a patch at the call inside the pointer hover because it leaves the function's bytes alone and returns the game's own viewport units. K's relay is gated, so K pays nothing on any other screen.

A self-check runs in the hook for the first ten seconds of each opening, once a second. It prints the game's point in the map's axes, P, the ratios a/Px, b/Py and c/Pz (which give the gain and the distance with no model), the viewport point from each input, K's half extents, and W's view and projection rows. That settles the pointer block's reading, the 1.83 and K's camera with no extra flight. Any other return address reaching K with a point within 10 units of the camera is logged once with its RVA: that is how a second test at click time shows itself.

**What each symptom becomes.** The arrow stays on a star whatever the head does; the star it covers is the one selected; moving the headset changes neither.

**Known limits.**
- Reach: the arrow's range is the UI camera's, about tan 0.546 (28.6 degrees) each side of the map's forward, where the hover's was about 45. Stars past it need the map turned. Dump 3's arrow is at NDC 0.94 on Orobriges, so this will be felt. Scaling the mouse would need a depth or mode switch for the panels, so it is not in this design.
- K returns false for a direction behind eye 0, so looking more than 90 degrees from the arrow clears the hover.
- System Map and Orrery get fixes 1 and 2, not 3, until their hover is read (CursorCircle, OrreryCursor).
- On foot: the VR map has no GuiFocus; stage 0's name may open the gate there, unproven.

## After the first flight

Log edvr_gfx_20261104_095046, build ca707034, dump 095453560001, whose 16 frames caught the head turning from yaw -15 to +9. Render 2254x2255, output 4508x4510. The head rows of the dump's first frames do not line up with their images, so measurements use frames 6 to 15.

**Icons and labels twist with the head.** Fix 1 fixed the triangles, not these. A record of an n=6 icon or a label carries a quaternion the CPU writes facing the head-free camera: rotate(q, ex) = -baseRight, rotate(q, ey) = +baseUp, rotate(q, ez) = -baseForward (544 records of 726 in the dump). Each quad is therefore a plane fixed in the world. Predicting each icon's top edge from that, at its own place in the image, against the measured edge: 0.5 to 0.9 degrees rms, with the mean edge measured going from -1.6 to -3.0 degrees as the head turned. In flat the map camera is the eye, so they face it.

The fix transcribes C31238331D8AC3D4 and 12382D2EA45E9632 (src\d3d11\galaxy_map_vs.h) and, for a record in the map's world whose quad is not flat on the galactic plane and not the identity quaternion, spans the quad on the eye's axes taken from the view-projection rows, with the signs that reproduce the game's when the eye is the camera. The quads on the galactic plane stay on it. Checked against the game's own bytecode on WARP through stream output: 4000 vertices each with both position encodings and bones, worst difference 0.000 for every record left as the game draws it. The rig tools\galaxy_map_vs_test pins the rest. They now match the triangles: upright in the eye, whatever the head does.

**The panels and the arrow.** The panels run as the game made them. The arrow draws at the panel's depth when its tip is on a panel and at star depth when it is not, by a transcription of 4EF6DDB075A927FA that takes the eye's origin out of a negative-scale record (the arrow alone has one; that shader also draws other sprites).

Deciding it needs the panels' coverage, which no signal gives (see Fix 3). Each eye has a mask, A8_UNORM at the layer's size, 40 MB for both eyes at this size, made at the first panel draw and let go when the map shuts. Every panel draw the layer took is issued once more into it with an accumulating alpha blend. At the arrow's draw, one more issue draws a single point at the arrow's record position, in the game's own depth so that it is compared on the panels' shell in that eye, through a pixel shader that discards under alpha 0.1, inside an occlusion query. The dump shows that position is exactly the arrow's tip. The answer is read at a later probe without waiting, so it arrives a frame or two late; either eye saying "on a panel" means on a panel. If the mask or query cannot be made, the arrow stays the game's.

The hover is unchanged: it follows the arrow's direction over panels too.

## After the second flight

Log edvr_gfx_20261104_105921, build bf17a80a, dump 110152762001. Sean: "the icons and text yaw with my head"; "The alpha mask works well, when the cursor is on the panel depth, it's great"; "lots of gaps between the panels": moving the mouse down the left rail drops it to star depth between the buttons.

**Correction to "After the first flight".** That reading of dump 095453560001 was backwards. The orange icons matched the world-fixed prediction because they were the stable ones. The ones locked to the eye were the n=12 triangles, spanned on rows 277 and 278: their top edges measure 0.00 degrees in frames 6 to 15, while the orange icons went from -1.6 to -3.0. So that build mixed two behaviours, and bf17a80a moved everything to the wrong one.

**Every map quad fixed in the world (BUILT, NOT FLOWN).** A quad's axes take nothing from the head's rotation: ray = normalize(quad - eye), right = cross(mapUp, ray), up = cross(ray, right), mapUp = stage 0's rows[1], where the tracked up lands. It applies to the n=6 icons and labels (the same faces test), the b span of the n=12 triangles, and the rings (94EA991C3A03DBD9, which also span on 277 and 278). The stems already build their width from the eye's position. Turning the head then changes nothing in the world, only moving it does, by parallax (6.7 cm at 53 units is 0.07 degrees). A quad is square-on when looked at, which the game's parallel planes are not off the map's forward. It stays level with the world when the head rolls, and the head does roll: -5.0 degrees at yaw 19.6 in this dump, an 11 to 16 degree tilt of the yaw axis in both dumps. Falls back to the eye's right within about a degree of mapUp, where there is no defined right. mapUp reaches the shaders at vertex constant slot 10, which none of the 329 vertex shaders in the game's dump declares. The rig holds every corner of an icon, a label, a triangle and a ring still to 2.6e-5 under 0.5 rad of yaw and 0.2 rad of pitch. Against the game's own bytecode: UI-frame records of the icon shaders exact over 4000 vertices; the ring's other outputs exact over 3200 instances, and its position within 1.19e-7 on an eye looking along the ray.

**The arrow bridges gaps between panels (BUILT, NOT FLOWN).** "On a panel" becomes: the mask covers the tip, or it covers a point within reach on both opposite sides along any of four axes (horizontal, vertical, two diagonals). The panels' outer edges stay exact, because a tip outside a panel has panel on one side only, and a gap up to twice the reach closes. Reach about 0.05 tan (110 layer pixels at 4508), so the rail's gaps and the rail-to-panel gap close and open map between panels stays star depth. It lives in the probe's pixel shader alone, so about 64 mask reads on one pixel. Rejected: a dilated mask, which also pushes every outer edge out; a time hold, which fails on a slow move along the rail; bounding boxes per panel group, which would need grouping the panels and cover open map at corners.

Other readings of this log: the arrow flipped depth 16 times in its first 4.2 s on the panels; panel draws after the eye's probe 22, 8, 0 in the first three windows (`maskLate`), so the mask is nearly always complete and a miss costs one frame's answer; the pointer's call ran 708, 2128, 502, 0 times per 30 s, where the first flight ran it every frame. Not explained, and no symptom reported: the map mode or a game gate in the pointer hover (doc above, `analysis\decomp\verify_20261104_galaxy_map_hover.txt` section 3) are the candidates.

## After the third flight

Log edvr_gfx_20261104_123510, build 6a12ef42, dumps 123832862001 and 123845396002. Sean: "Cursor around the UI panels/buttons works well now and the icons do not shift with head movement"; icons "rotate their orientation (most noticeable with the triangular icons) based on the camera shift of the X axis. They should always be pointed down towards the flat grid plane"; "the cursor when moused over an icon seems to jump up and down in space".

**The arrow's jump is its depth flipping (MEASURED).** Across dump 123832862001 the arrow's record is still to 0.0001 at P (-0.043, 0.151, 1.021), yet between frames 5 and 10 the arrow moves about 30 pixels against its star on the half-size image, what the 0.027 tan between star depth and panel depth gives. Record 1258, drawn by B10B032BDFD46700, is the hover halo: scale 11.8 at (-0.308, 0.956, 6.554), tan (-0.047, 0.146) against the arrow's (-0.042, 0.148). So it covered the tip in the mask. B10B032BDFD46700 is out of the mask; C4B4B3 and 5DAD41 stay.

**Sprites point down the grid's vertical.** With the camera's up a quad is exactly upright in the image at any place in the view when the head is at the map's rest pose, while the stems lean toward the nadir. The triangle's record stands on the plane with its apex at the star's foot, so its axis should be the stem's. The icons (C31238) and rings (94EA99) now take up = galactic up, handing over smoothly to the camera's up for rays within 35 to 16 degrees of the vertical, where the lean has no meaning and a quad would spin about the nadir. Labels (12382D) keep the camera's up, because text fanned out by 26 degrees at tan 0.5 is hard to read. At the map camera's forward both give the same axes. Rig: an icon's up edge is 4e-7 rad from the projected vertical; the largest step through the hand-over is 9.4e-3 NDC between rays 0.25 degrees apart.

Also read: the "another caller" line named only the stars' own call (see Ruled out), so it is gone. `its up written` runs 246 to 2309 times in 30 s while the map moves.

## After the fourth flight

Log edvr_gfx_20261104_132124, build 759ec617, dump 132601814001, Pimax Crystal Super, 2016x2017 in, 4032x4034 out. Sean: "I think this basically works"; "the system icons seem to wobble"; "performance while in the galaxy map drops precipitously on my 5090. From 90 fps down to sub 45"; "Could we also normalize the text so it's parallel to the flat grid below?"

### The frame rate

(This section's conclusion was wrong: see "After the fifth flight". The table's patterns D and E are a path the game never takes.)

MEASURED. `native timing CPU`: 450 frames in 10 s on the map against 900 on the build before; transfer 5 ms against 0.2 ms, so the GPU was behind, and `draw hook CPU` rose only from about 1.1 to 1.56 ms. A micro-benchmark on this RTX 5090 (560 sprites a frame at 4032x4034, GPU ms by timestamps):

| Pattern | GPU ms |
|---|---|
| Colour, stencil write-back on the game's depth-stencil (third build) | 0.361 |
| Colour with D16 test, write-back, depth issue with no target (fourth build) | 5.530 |
| Colour with D16 test, depth issue with no target, no write-back | 0.494 |
| Colour with D16 test, depth issue with the target kept at write mask 0 | 0.484 |
| The same with the write-back | 5.561 |

Alternating the private D16 with the game's depth-stencil per draw is the whole cost. The write-backs are off for the map's sprites in VR: the only draws that test stencil are FC1193AFFC596F74 at #1 and #2, and each eye's depth-stencil is cleared, stencil included, immediately before them (`D dsv=@44 f=3 z=0 s=0 q=127`, then q=128), so the bit a sprite writes is cleared before anything can read it. The depth issue keeps the target bound, which also costs less CPU (0.27 against 0.40 ms).

### The wobble

The game's eye in the map frame is float32 at about 50000 units, one step 0.0039, and moves in several-step jumps: x steps of -0.0078, +0.0118, -0.0079 between frames, peak 0.006 against a smooth fit, which is 2.8 pixels at 5 units on the 4032 image, 1.4 at 10, 0.7 at 20. Image motion measured by phase correlation on an icon at 3.8 units swings up to ±4 pixels between frames (a smooth eye gives ±2), and the size matches the rounding (3.25 pixels rms). The frame-by-frame signs do not all match the dump's `now` rows, whose rounding need not be the draw's, and icons at 12 to 16 units fit the smooth eye better than the rounded one. So the amplitude is MEASURED and the mechanism is INFERRED.

The precise parts exist. Stage 0's position is constant at rest, and the UI frame's two cameras hold each eye's offset in float32 near zero (camera+0x50 in the refresh hook, kind 5): map eye = position + o.x*rows[0] + o.y*rows[1] + o.z*rows[2] within 0.0015 for both eyes in this dump. The icons, labels, rings and stems now position from (W - position) - offset, exact for near values, whenever the game's eye is within 0.04 units of one of the two (rest), else from the game's own eye (a moving map, whose pose can lag a frame). Stems needed their own transcription (3028E7C6E0E097F9). Rig, with 3 steps of noise on the eye 4 units away: spread 0 NDC, against 2.9e-3 to 3.8e-3 with the game's formula. Stars and sky stay the game's, so a very near star's glow can still jitter under its steady icon.

### Text parallel to the grid

The labels take the same up as the icons, so a baseline's direction, cross(gridUp, ray), lies in the grid's plane; rig: its y component is at most 1e-6 of its length. Labels on the plane stay on it. The cost is the one named when this was first decided the other way: text fans out by about 26 degrees at tan 0.5, with the grid's own lines.

## After the fifth flight

Log edvr_gfx_20261104_141340, build e02c6248, dump 141733701001. Sean: "moving the camera left/right causes the icon and text to rotate around its y axis, also performance is still quite poor"; on main (11ef91d5, Epic, edvr_gfx_20261104_142228), "the galaxy map there runs at 90 fps mostly". The adversarial review is `reviews\galaxy-map-perf-review-2026-11-04.md` (git-ignored); its benchmark is in the session's scratchpad, not in the repository.

**The frame rate.** `uiLayerEnd` runs `restore()` after every issue (vscreen.cpp:4676, ui_layer.cpp:2217, read and confirmed), so one sprite is layer+D16, draw, game+D32S8, layer+D16, draw, game+D32S8. A benchmark of that real sequence on this RTX 5090, at the dump's 432 sprites an eye, with the game closed: 11.1 ms as it runs, 1.0 ms with one bind for the run, 0.92 ms for the third build, 0.10 ms for main; 3 times the sprite size gives 11.6 ms, so it is not fill. MEASURED in the benchmark; that it is the whole miss is INFERRED, since sprite GPU time has never been timed in the game. One unknown, about 8 ms for the rest of the frame, fits all three builds' frame rates. Fix 2's "Cost" line had already named the answer: bracket the contiguous run with one target switch.

**The rotation.** Reports against builds are tabled in the review. What Sean has asked for is orientation that follows neither the head nor the camera's translation, in a fixed relation to the grid. Proposed: one pair of axes a frame shared by every world quad, with no ray, eye or position term. R is the camera's horizontal, in the grid's plane. U is either the grid's vertical (along the stems, scaled to keep height at the map's centre) or the camera's up (the game's own planes, with the triangles joining them), carried on one temporary key so one flight decides. Cost of `camera`: a quad off the map's forward foreshortens by the cosine of that angle, 0.89 at tan 0.5, as the game's planes always did. Cost of `stems`, corrected by the second review (this line first gave 0.89 for both): that plane stands vertical while the camera looks 45 degrees down, so a quad at view tangents (x, y) has 1 + y times its height at the centre and shears by atan(x / (1 + y)). That is 2.0 at y +1, 0.5 at -0.5, 0.2 at -0.8, edge-on at the nadir, and 45 degrees of shear at (0.5, -0.5). So `camera` is the default. In exchange a triangle off the view's middle leans off its stem by atan(x / (1 + y)), 27 degrees at (0.5, 0). OPEN: 759ec617 had the same icon formula and drew "basically works" with no rotation report.

**Also open.** The wobble fix is unproven: this dump cannot test it and the report does not mention it. The stencil removal's proof covers raster draws only, not a stencil-plane SRV or the 36 dispatches a frame.

**The course, built as the sixth build (NOT FLOWN):** one bind for the run, covering every owner draw path, EDVR's own readers of the targets, and the frame boundary; the shared axes with the temporary key; gate on `native_frame_cycle_window` and `xr_end_frame` with `advanced.app_gpu_timing` on and one press of the GPU pass profile on the map, plus depth-stencil binds a frame in the 30 s line and a rig test that fails on more than two binds a run. Then decide the sprite depth (about 0.9 ms, 65 MB) and whether `mapEyeRel` and the stems' swap stay, on the evidence. Correct the false comment in `ui_layer.cpp` and `galaxy_map_ui.h`, and move this doc's ten pinned build addresses (CODING_STANDARDS; not done yet, before merge).

**The sixth build.** A run is held from a map sprite's restore until something other than a map sprite needs the output merger (`map_run_core.h`, `tools\map_run_test`). The axes are one set a frame from stage 0's rows (`galaxy_map_axes.h`). A second adversarial review read the code. It traced the real path by hand and found no per-sprite bind: the dump's census gives one run an eye, 433 to 435 draws, 2 binds a frame. Its benchmark of the sequence as built, GPU ms on this RTX 5090, fifth build against sixth: 8.22 and 0.76 at 326 sprites an eye, 11.02 and 0.95 at 432, 12.26 and 1.08 at 485. Predicted frame about 8.9 ms, so 90 fps: the benchmark is MEASURED, the frame rate INFERRED. It raised these, all taken in this build: the `stems` default above; a `OMGetRenderTargets` read then set inside a run would bind the layer as the game's target, since nothing hooks the read (now kept, and counted as `of the layer's own read back` in the 30 s line); and a context-state swap now ends the run rather than dropping it. Stage 0's right is (1,0,0) in all twelve dumps checked, so `camera` baselines run with the grid at that pose; no dump is at an orbited pose. Not checked: the game's own sets of targets inside a run, the game's 116 candidate reads of the targets, EDHM or 3Dmigoto overrides, GPU cost in the game itself.

A dump's census names the layer and the D16 for draws inside a run (`draw_census.cpp`, `object_probe.cpp`), so an `r=` filter stops matching there. No state is harmed; read a dump knowing it.

## After the sixth flight

Logs edvr_gfx_20261104_163752 and edvr_openxr_20261104_163753_518_38980, build 5086b6f3, right build confirmed. The map was open 16:40:57 to 16:41:40, 16:41:42 to 16:41:50 and 16:41:53 to 16:42:08, about 66 s. No dump. Sean: "Ran it, I prefer camera". The map was opened once with `stems` (16:41:42, about 8 s) and otherwise with `camera`, as the log's opening lines show; the menu's five flips of the key took effect only at the next opening.

| Reading | Fifth build | Sixth build, map open | Gate |
|---|---|---|---|
| Frames per 10 s (`native timing CPU`) | 397 to 618 | 900, 876, 900, 900, 859, 877, 895 (the lows span a close and reopen) | near 900 |
| `native_frame_cycle_window`, fps | not read | 88.07 and 88.67 over the two 30 s windows holding the map | near 90 |
| `xr_end_frame` p50 in `native_submit_phases` (the runtime's log, not the gfx log) | 5.09 ms | 0.22, 0.20, 0.22 ms | under 1 ms; 0.21 on the third build |
| Render target binds for the sprites | 2,600 to 3,500 a frame | 2.0 a frame | about 2 |
| GPU pass profile, frame busy | not read | 8.97 ms, 10.04 ms in the reference frame with no delay | predicted about 8.9 ms |

So the frame rate is back, and the sprite binds were the cause. MEASURED in the game.

The card's reading of `of others` was wrong. It is 4394 of 4394 runs and 4636 of 4636: every run ends with the game setting other targets, which drops it with no bind back, and that is the clean ending. So "high" is normal when it equals the runs. `read back` and `kept` are 0 in both windows: the game neither read the layer's targets back nor set its own again inside a run.

`app_gpu_timing` went back to commented in the Frontier ini after this flight.

**The wobble.** Asked after the flight, Sean: "Wobble is gone". So `mapEyeRel` and the stems' swap stay. The flight cannot separate them from the shared axes, which landed between the report and this answer. The rig gives the mechanism: three steps of float32 noise in the game's eye move an icon 4 units away by 3.44e-3 NDC under the game's formula, about 7 pixels across the 4032-wide output, and by 0 with the precise parts (G11). A flight without them would only test that, so none is planned.

## The System Map

Evidence: dump 050358167001, build 80c56505, Frontier, native OpenXR, 2016x2017 in, 4032x4034 out, DLSS. Stage 0 is named `SystemMapView` (so the Open line above is answered), rows (1,0,0), (0,0.087,0.996), (0,-0.996,0.087), position (18.64, 16.19, -1.41). The bodies lie on the grid plane about 16 to 30 units from the camera.

**The cursor. MEASURED.** The arrow is the Galaxy Map's: the same record shape (s = -0.422, |W| = 1.036) in a UI frame on the same axes. On 38 Lyncis 1 the near arrow's offset from the body is +0.066 NDC in the left eye and +0.021 in the right, 91 pixels apart at 4032. At star depth it is +0.035 and +0.038, 6 pixels apart, which is why a far arrow reads as being at the planets' level at 16 units too.

The hover is K again, from a second call site (`docs/build-334925.md`, "VR Galaxy Map"). The System Map's per-frame step projects the same pointer block's +0x30 through the same K and hands the answer to the store's hover and select, and those compare it in viewport x, y with each body projected through K, radius at least 0.03 of the height. So a point far along the arrow's ray picks by direction alone, as on the Galaxy Map. Projecting that far point through the map camera gives (0.2487, 0.0032) left and (-0.1081, 0.0032) right, the star-depth arrow's NDC in each eye to four places, so the arrow and the hover agree. INFERRED: that K's view 0 is the head-free stage camera here too. If it is an eye, the error is that eye's offset over the distance, about 0.003 tan, far inside the pick's radius.

The change: the pointer's gate opens on either map; K's detour takes both return addresses; the arrow's swap has its own load. The fixed icons, labels and sprite depth stay the Galaxy Map's. The Orrery shares the controller (INFERRED), and gets the same if its stage is named.

**The cursor with Anti-aliasing off.** The regression was `35a97739` on `claude/rc7-review-fixes`, flown in 80c56505 and not on main: with the layer down the pointer stood aside, so the hover went back to the game's near point. Its reason stands: the mask and the probe ran only inside draws the layer took, so the arrow had nothing to say it was on a panel. Now both run from the game's own issue when the layer is not live (galaxy_map_ui.h, THE MASK WITHOUT THE LAYER), with a mask the size of the target, and the pointer's gate takes no term from the layer. That commit must not land as it is: merging that branch has to drop its `pointerWanted` gate.

**The two markers. Cause OPEN.** Flat's screenshot and census (log `edvr_gfx_20261105_062523`, 06:28:33, 1920x1080, build f379c288) against the VR dump. World sizes are measured against the 3.29 units between the two bodies' centres, in each picture.

| | Flat | VR | Flat / VR |
|---|---|---|---|
| 38 Lyncis 1, surface | 0.885 | 0.886 | 1.00 |
| Planet at (15.35, 0, 0), surface | 0.785 | 0.79 | 0.99 |
| The X icon's width | 1.00 | 1.05 | 0.96 |
| Atmosphere ring on 38 Lyncis 1, outer radius | 1.14 (its atmosphere's outer radius is 1.15) | 0.56 | 2.04 |
| Landable arc on the planet at (15.35, 0, 0) | 1.424 | 0.687 | 2.07 |

The markers are 12382D sprites (PS 855C46) from 1024x1024 atlases, one sprite on each marked body, and the sprites' meshes split the modes. INFERRED that these are the ring and the arc: the bodies and the counts match, nothing else in both draw lists does, and the ring and arc are in the scene where the white hover halos (B10B03, UI frame) are not. Index start and base vertex in each census:

| Kind | Flat | VR |
|---|---|---|
| Atmosphere ring, 9 bodies | 8 on (736, 368), 1 on (744, 372) | 9 on (7248, 1734) |
| Landable arc, 7 bodies | 6 on (776, 388), 1 on (784, 392) | 7 on (7288, 1754) |
| Settlement ticks, 6 bodies | 6 on (216, 108) | 6 on (6728, 1474) |

The same pool, loaded 6512 indices later in VR: all three kinds sit at that one offset, so VR's meshes are flat's common ones, and VR draws nothing on the next mesh along. VR's sizes fit the moons (surface 0.43, atmosphere 0.556) and flat's fit these two larger planets. So this reads as a choice of mesh variant per body rather than a wrong scale. Not proven: flat's record scale s is unread (VR's is 0.88 on every marker), and neither mesh's size has been read.

Static RE (`analysis\decomp\verify_20261105_system_map_hover.txt`; addresses in `docs/build-334925.md`) did not find the selector. There are 25 `SystemMapPlanet_*` models in five variants each (`Atmospheric`, `LandableArc`, `LandableCity`, `PermitArc`, `PermitCity`), loaded by name in one switch into a set nothing in the executable was found reading: no reader of the set, no name built at run time, no ladder from the body's radius to an index in the System Map's node code, and no camera or viewport read outside the pick. So the choice may be the engine's, acting on the assets' own data. The engine has a LOD manager, whose selection was not found, and a settings block holding `LODDistanceScale` beside `HMDRenderTargetMultiplier`. Whether either feeds this is OPEN.

**The zoom test (dumps 072734614001, camera height 79.7, and 072747469002, height 12.25, fully out and fully in, build 408e7636).** VR draws the same meshes at both ends: rings 9 on (768, 384), arcs 7 on (808, 404), ticks 6 on (248, 124). The ring on 38 Lyncis 1 measures about 0.58 at full zoom in, against 0.56 at height 16, and the ring on a moon about 0.67 at full zoom out at low precision, so the world size does not follow the zoom. That rules out a LOD on distance or on on-screen size holding VR at this variant by range alone. What holds it is still OPEN, flat's zoom has not been looked at, and whether the size is in the mesh or in the record's scale is unread (the dump keeps no vertex streams for this shader, and these two dumps hold no pool files).

**The fix, a measured rule rather than the game's selector.** Each sprite is drawn k times larger about its own position, in a replacement for 12382D on the System Map (galaxy_map_vs.h, THE SYSTEM MAP'S MARKERS): k = surface radius / 0.4355, never below 1, at most 4. The ring then sits at the atmosphere's outer radius (1.139 on 38 Lyncis 1, flat's is 1.14), the arc on the planet of surface 0.79 at 1.246 (flat's is 1.424: outside the limb, 12% smaller), and moons stay as they are. One k for all three kinds, because telling them apart in the draw has no clean signal. The radius comes from the same per-body constants as the atmosphere pass, cb1[125] and cb1[126], read at the Unmap of a 5376-byte constant buffer into a table at vertex slot 11, so another build's size leaves the markers as the game drew them. INFERRED: that rocky bodies write those rows too (their pixel shader reads them). If not, the planet at (15.35, 0, 0) keeps its small arc and the log's body count shows it.

**Everything but the bodies out of the AA pass.** Already in the layer on the System Map: the markers and labels (12382D), the icons (C31238) and the stems (3028E7). Left in the scene until now: the plane's X and diamond icons (88DCF1, FEDE6C, n=6) and the grid (same pair, n=1014). Both now go to the HDR layer while the System Map's camera is up. The icons come after the bodies and keep their depth test, against the layer's seeded copy. The grid and the stems come before most bodies at viewport depth 0, and the game relies on the bodies covering them, which a layer composited over the scene cannot do. So the grid takes no test, and at the first draw after the bodies the layer is cut back to clear wherever the game's depth holds anything (galaxy_map_vs.h, THE CUT-OUT BEHIND THE BODIES). That also stops stems running over the gas giant. Left over: small moons write no depth (dump 072747469002), so the grid lies over them; planetary rings write none either, so the grid and stems lie over those; and the edge of a cut-out is at the eye's render resolution. The planets' rings (1F3AD1) and 6041FD stay in the scene as part of the bodies.

### The System Map's click

FLOWN 09:20 (build fc05ee07), Sean: the atmospheres are fixed, and the one difference left is that clicking a body does not select it, nor a second click move the camera to it. MEASURED in the code (`analysis\decomp\decomp_3AF9060.txt`, `decomp_3B46A40.txt`): at the button's press the map's step keeps the pointer's K answer, and at the release it calls select-at-pointer only if the answer is exactly equal in x and y, bit for bit. Select-at-pointer does both jobs: it picks the body and sets it, and if that body is already the target it moves the camera there. So both symptoms have one cause.

The answer was never equal in VR. The cursor's position runs through the head: over 19 frames of dump 050358167001, mouse still, the arrow's W.z moves 6.6e-4 and its direction about 5e-6 radians, tracking the eye, and it is never the same two frames running. In flat a still mouse gives the same bits. INFERRED: this is the game's own VR behaviour, not EDVR's, and the far-point hover neither made nor mended it (the far point only passes the wander on).

The change: on the System Map's call the K answer is held while the map's camera has not moved (`mapCameraChanges`, which counts a change of a single bit in the head-free pose) and the arrow's direction stays within 5e-4 radians of where it was taken, about 0.03 degrees and three times the 1.7e-4 measured while the head yaws 8.7 degrees. The mouse decides when it changes, and the drawn arrow is untouched. It is below the pick's own radius, at least 0.03 of the viewport's height. A mouse step past it during a click cancels the click, as in flat. The Galaxy Map's call is not held: its clicks work.

### The close view of a body

FLOWN 10:07 (build 22eb5c87), Sean: clicks work. Zooming into a landable body gives a 3D view where the arrow's depth looks wrong against the settlement icons, and it looks a little jagged while zooming. Dumps 100700560001 (at rest, camera 3.2 units from a body of surface 0.42) and 100710371002 (moving, 8.3 units).

**The arrow. MEASURED.** It was at infinity, right for the stars and near enough at 16 units, where a body is 7 pixels of disparity from infinity at 4032. At 3.2 units the body is about 33. So on this map the arrow now goes to the nearest known body's surface: the VS takes the eye term back scaled by |W| / D, which keeps its size and ray and gives it the disparity of an object at D. D is the distance from the map camera to the nearest body's centre less its surface radius, in the bodies' table, once a frame, floor 0.25 (galaxy_map_vs.h kMapArrowDepthVS). In the regular view that is about the camera's height, so the arrow gains the plane's disparity there too. The Galaxy Map stays at infinity. INFERRED: a settlement on the limb is nearer the body's centre plane than D says, by up to its radius; that is 15% at 3 units.

**The jaggedness. Two things, one left OPEN.**
- Dump 100710371002 was taken moving: 13 of its 16 frames have the DLSS history reset, on purpose (Fix 2: while the map's camera moves its history cannot be reprojected, because the pass has only the head's delta). The planet's limb at 4x looks acceptable. The scene's other edges in those frames were the lines round the body, VS 1DEF9D and BA1606 with PS 337583, which this build moves to the HDR layer with the plane's icons (depth tested, after the body). The planet itself still has no history while the map moves.
- The settlement icons (12382D sprites) already in the layer break up when small: about 14 pixels across in that dump, against a clean 40 at rest. It does not depend on motion. ruled out: EDVR's mip bias, because this install's samplers are all bias +0.00 (`texture_lod_bias = 0`; the census at 10:04:06). Open: whether the atlas has mips at all. The build logs `the System Map's first marker: its atlas view has N mip level(s)`. If 1, the icons minify with nothing to fall on, and supersampling them is the fix, which can wait for that line and for Sean's word on which of these he means.

### The close view's surface, arrow and reticule

FLOWN 10:33 (build 85e2fe00), Sean: better while zooming, but some textures on the surface flicker, the arrow floats too high, the reticule renders inside the planet, and could the settlement icons go in the UI layer. Dump 103332590001: Pimax, native OpenXR runtime, DLSS performance preset K, 2016x2017 in, 4032x4034 out; camera 2.40 units from a body of surface 0.418.

**The arrow. MEASURED.** The log's `arrow's depth` read 1.71, then 0.78. The surface under the arrow was 1.98 away, and the record's measured disparity agrees with about 1. The rule took the nearest of all known bodies, and that one was behind the camera. The arrow's eye term is in map units: its eye baseline matches the map's to four figures (0.05509), and a disparity at D matches the map's at the same distance (D 1: 0.4059 against 0.4057). So the depth now comes from the arrow's own ray (galaxy_map_ui.h rayBodyDistance): the nearest shell it hits; failing that, the point of closest approach to the body it passes closest to in angle, which meets the hit's distance at the limb. Bodies behind the camera do not count. 1 / D is 1 / distance + 0.04, so it floats the same 3.8 pixels of disparity at 4032 at any zoom, and it eases toward a new depth so a jump between bodies does not pop.

**The reticule. MEASURED.** C31238 with PS 539A48: its record stands at the focus body's centre, and it lands on the image's reticule in both eyes to within a few pixels at 4032. Its stereo depth is therefore 2.40, inside the planet whose surface is at 1.98. Depth test off, so it draws over the planet and reads as being in it. kMapReticuleVS gives it the arrow's treatment on the System Map: the eye term scaled so it keeps its place and size from the camera and sits at the same height, shell less 0.04 in 1 / distance. The camera is in MapBodies's new row. A record at no known body stays the game's.

**The settlement icons are already in the layer. MEASURED.** Five settlements in that frame, each with two draws at the same record position, 1.0157 of the surface radius: a surface-tangent quad, 88DCF1 with FEDE6C (s 0.096, in `map plane icons`), and a billboard marker, 12382D with 855C46 (in `map canvas`). The layer's 30 s line counts 10 canvas, 2 sprite and 22 plane draws a frame, which is every one of them in both eyes, and the zoomed final image shows them sharp at the layer's resolution. So nothing moved: what read as wrong there was their depth against the arrow and the reticule. The marker's atlas has 11 mips and anisotropic filtering at bias 0, so ruled out: no mips as the small markers' break-up.

**The flicker. MEASURED, the fix not flown.** All 16 frames of the dump reset the history, with no jitter: the map camera moved about 12 mm a frame with the head still (0.1 mm). So each frame is one half-resolution frame upscaled. The surface has a patch of fine bright terrain whose pixels differ on each raw frame, and the upscale can do nothing with them. The dump at rest (100700560001) kept history on all 16. The reset is there because the pass had only the head's motion: `MV.bin` over the planet is a near-constant (0.17, -0.41) pixels, the head's rotation, since a body 2 units away is inside the 10 m the pass gives the ship. On the other hand the camera's rows do describe this scene. Warping a frame with the rows and the sphere and comparing it to the frame before, over the 85,000 planet pixels:

| Frame | Mean move, pixels | RMS, rows | RMS, no motion | RMS, head only |
|---|---|---:|---:|---:|
| 13413 | 3.16 | 4.58 | 13.57 | 17.35 |
| 13417 | 1.13 | 4.53 | 7.74 | 8.05 |
| 13421 | 0.97 | 4.60 | 5.27 | 5.18 |

The rows' figure stays flat as the move changes, and what is left of it is largely that same fine terrain, sampled again at another position. So in the close view (a body in front whose radius is at least 0.05 of its distance, galaxy_map_ui.h bodyFillsView) the split goes to a millimetre, as on foot, so every pixel with a depth takes the rows, and the history and the jitter stay through the move. The shader needed nothing. The far view still resets: its small moons (E79BF3) write no depth, and the rows would move them as though at infinity. A moon in view during a close-view drag can trail for the same reason. INFERRED: DLSS on these vectors will hold that terrain still. The warp shows the vectors are right, but only a flight shows DLSS is content with them.

### The side buttons

Sean, 2026-11-05, after the merge: on both maps the cursor falls off the columns of button icons at the left and right, and they are hard to click. He asked for the panel alpha mask there.

**They are already in the mask. MEASURED, with one inference.** The System Map dump (103332590001) has only the panel shells (C4B4B3 and 5DAD41, n=360), a few n=6 quads and the arrow in the UI target, none of them per button, so the columns are part of the shells' textures. The button's fill is at least 83% opaque (Galaxy Map dump 141733701001: the backdrop's blue channel falls from 42 in a gap to 7 under the fill), against the mask's threshold of 10%. INFERRED: the mask's alpha is the alpha that gets drawn; no dump captures the mask. The gaps between buttons are about 24 pixels at 4032, which the bridging's reach of 95 closes.

**What it most likely is. INFERRED from those sizes, not measured in play.** A button is about 165 pixels wide. The bridging needs mask on both sides, so at the column's outer edge nothing holds the arrow, and past that edge it goes to map depth and moves 50 to 100 pixels in each eye (the eye offsets of 0.03 to 0.055 over the arrow's distance of 1.04, at 1894 pixels per tan). At map depth the arrow is also not where the game draws it, and INFERRED, not where the game tests for a button.

**The change.** While the arrow is on a panel, the probe is kMapProbeStickyPS: mask within the same reach on either side keeps it on, about 3 degrees. Getting on is still the exact test, so the arrow does not switch early as it approaches a panel. The sticky test says yes wherever the exact one does, so the two cannot alternate at one place. A star right beside a panel can still be hovered, since hover follows the arrow's direction, but the arrow reads at panel depth until it is clear. It applies to every panel on both maps, with Anti-aliasing on or off.

If the buttons are still hard to click with the arrow steady, the next question is the game's own hit test for them, not the depth.

## Flight card, the side buttons (flown 12:23)

Build dabfff15. VR, both maps. Sean: "That fixed it", so the depth was the cause and the game's own hit test needs no look.

1. Slide up and down a button column, and overshoot it sideways by a little. The arrow should stay at the buttons' depth with no jump, and the buttons should click first time. Log: `probes made N (M with the sticky edge)`, with M above 0 once the arrow has been on a panel. M of 0 means the path never ran.
2. Move well away from a panel: the arrow should drop to map depth about 3 degrees clear of it. Moving toward a panel, it should not switch until it reaches the panel.
3. Hover and click a star or planet right beside a panel. Selection should work; say whether the arrow's depth there bothers you.
4. Anti-aliasing off: the same on both maps.

## Flight card, the close view's surface (flown 11:17)

Build 852102d1. VR, Anti-aliasing DLSS.

1. Zoom into a landable body with settlements, then drag and zoom. The speckled terrain should stop flickering. Watch for trails behind small moons, and for the planet's edge ghosting. Log: `temporal aa map: ... N of those kept were the System Map's close view moving`. N of 0 means the path never ran.
2. In that view: one arrow, not two, just above the surface and above a settlement icon, at the same level across the disc and off the limb. Log: `arrow's depth` a little under the distance to the surface under it, 1.9 for 1.98.
3. The reticule at the same height as the arrow. Log: `reticule lifted` above 0, 2 a frame.
4. Regular System Map: the arrow still reads at the planets' level; no new smear, since that view still resets. Galaxy Map: arrow and icons as before, with `arrow's depth` 0. Clicks still work on both.

## Flight card, the close view

1. Zoom into a landable body. The arrow should read at the level of the settlement icons: one arrow, not two, when you look at an icon. Clicks still work. Log: `arrow's depth` about the camera's distance to the surface, 2 to 3 units here.
2. Regular System Map: the arrow still reads at the planets' level. Galaxy Map: arrow as before; log says depth 0.
3. Zoom while looking at the lines round the body: they should stay sharp. Say what still looks jagged: the small settlement icons, or the planet's edge while it moves.

## Flight card, the System Map's click

1. System Map: click a planet. It should select, as in flat. Click it again: the camera should move to it. Try a moon, a planet and the star.
2. With the mouse still and the head moving, the hover should not flicker between neighbours. Drag the map with the button held: it should drag and select nothing.
3. Galaxy Map: selection as before.

Log: in the `map pointer` 30 s line, `the System Map's held and answered unchanged` close to the call count while the mouse is still. If that is 0, the hold never applied: the map's camera is moving every frame or the arrow's direction is wandering past the limit.

## Flight card, System Map and Anti-aliasing off

Environment: Frontier, native OpenXR, whichever headset; DLSS on unless the step says otherwise. First check the build: `python tools\edvr_log.py --dir <Frontier>\edvr_logs --expect-build HEAD`.

1. System Map, AA on. Put the arrow on a planet, off the panels. Expect: the arrow reads at the planet's level in stereo, with one arrow and not two; the planet under the tip is the one hovered and selected; moving the head does not change which. Run onto a panel and off: one jump in depth at the edge. Log: `map pointer: a map opened (the System Map)`, and in the 30 s line the projector call about once a frame, replaced, and `arrow 4EF6DD` counted. A `0 replaced` means the System Map's call was not reached.
2. Galaxy Map, AA off (F7, or the F8 menu). Expect: the cursor selects stars as with AA on, and the arrow is at panel depth on panels and star depth off them. Log: `galaxy map: the UI layer is not taking the panels`, and in the 30 s line panel draws issued into the mask and probes made and answered, all above zero. All zero means the path never ran.
3. System Map, AA off: the same as step 1.
4. Galaxy Map, AA on: 90 fps, and selection as before.
5. The markers. System Map, AA on: the ring on a large planet should sit just outside its limb, and the arc outside the limb of a landable planet, moons as before. Log: in the 30 s line, the markers scaled and the bodies known. A low body count, or an arc still small on a rocky planet, means those rows are not written for it.
6. Out of the AA pass. System Map, AA on, zoomed fully out: the grid and the X and diamond icons are as crisp as the labels, and stay still when the map stops moving. Grid and stems stop at the star's and planets' edges and do not cross them. Log: `map plane icons` and `map grid` in the layer's 30 s line, and cut-outs about two a frame. Look for grid lines over small moons and over planetary rings, the known leftovers. Insert dump.
7. The Orrery, if it is quick: its draws have not been read, and it may share the System Map's camera name, so check nothing is misplaced there.

## The merge with main

Main was 168 commits ahead, including PR #166's draw path. Five files conflicted, all because both sides had added to them. The signature table and the two expectation files were regenerated with `hook_signatures.py`, not merged by hand. Two failures came with no conflict:

- `pureDrawReissue` gained a caller argument on main: a compile error at three of this arc's call sites, and one source pin.
- `uiLayerFamilyHdrVsMayWant`, main's skip of the UI family test for vertex shaders the rule never names on the HDR target, did not list the map's four overlay shaders, so every map sprite would have stayed in the scene. Good fps, the smear back, no sprite depth, and `0 runs` in the 30 s line, with the build still green. A review of the merged tree found it before a flight. Fixed, and `sharp_weapons_test` now sets `mapOpen` and pins the four.

The same review read the merged draw hooks, fast paths, tracked bindings and output merger readers against the run, and found them sound. That is reading, not a run: the 88 to 90 fps and 2 binds above are from before the merge.

**Flown on main.** Log edvr_gfx_20261104_183551, build 5bce272d, right build confirmed. Sean: "Flown, push it". The map was open 18:37:55 to 18:38:42, about 47 s: 2.0 render target binds a frame, runs started 2.00 a frame, `of others` equal to the runs, `kept` and `read back` 0, no `-- 0 runs` tail; 899, 901, 891 and 900 frames per 10 s; `xr_end_frame` p50 0.20 to 0.22 ms. So the sprites are in the layer and the run holds on main's draw path. The rest of the flight had no fault lines: in VR 87 to 89 fps over each 30 s window, with single 10 s windows down to 84, then 88 to 90 fps in flat after a live switch at 18:39:36. How the map looks is from Sean's word alone: no problem reported, and no dump.

## Flight card, merged main (flown 18:35)

Right build first: `python tools\edvr_log.py --target <odyssey path> --expect-build HEAD --version`.

1. Open the map. Expect 90 fps, and lines and text crisp while the map moves. Smear here means the sprites have left the layer.
2. Check the rest still holds: icons and text do not rotate, near icons cover far ones, hover and selection, the panels and the gaps between them.
3. Fly a normal stretch away from the map too: this is the first flight of this work with main's 168 commits.

Log gates: the `map pointer:` 30 s line with render target binds about 2 a frame and runs started about 2 a frame, and no `-- 0 runs` tail; `of others` equal to the runs is the clean ending; `xr_end_frame` p50 in `native_submit_phases` (the `edvr_openxr` log) under 1 ms.

## Flight card, sixth build (flown 16:38)

Right build first: `python tools\edvr_log.py --target <odyssey path> --expect-build HEAD --version`. `advanced.app_gpu_timing` is on in the Frontier ini for this flight.

1. Open the map, read the fps. Expect 90, as the third build held and main mostly holds. Press F4 once on the map for the GPU pass profile.
2. Pan left and right, then orbit, then turn the head. Expect: icons and text do not rotate. Look at the bottom of the view: nothing flattens.
3. Look at the triangles. With `camera` they all point the same way and lean off their stems away from the view's middle. Is that acceptable?
4. Compare: set `galaxy_map_icon_up = stems` in `[experimental]` in the Frontier `edvr.ini` with the game running, close and reopen the map. Expect: triangles along their stems, flatter towards the bottom of the view. Say which is better; the other goes.
5. Hold still and move the head slowly near a close star. Do nearby icons still wobble? This decides `mapEyeRel` and the stems' swap.
6. Check nothing was lost: hover, selection, the panels and the gaps between them, the bookmark behind the triangle stays hidden. Insert dump.
7. If sprites land in one corner of an eye or draw over the wrong thing, that is the run: say so and note the time.

Log gates: `xr_end_frame` p50 under 1 ms (0.21 on the third build, 5.09 on the fifth); few late frames in `native_frame_cycle_window`; the `map pointer:` 30 s line with render target binds about 2 a frame, runs started about 2 a frame, `of others` and `read back` near 0 (a high `of others` means the game breaks runs; a `-- 0 runs` tail means this never ran); `galaxy map: experimental.galaxy_map_icon_up = ...` at each opening.

## Flight card, fifth build (flown 14:13)

Right build first: `python tools\edvr_log.py --target <odyssey path> --expect-build HEAD --version`.

1. Open the map, read the fps. Expect 90 as on the third build. Pan and orbit.
2. Hold still and move the head slowly near a close star (zoom in till one is about 4 units off). Expect: its icon, label, ring and stem steady together. Insert dump.
3. Look across the view at labels. Expect: baselines run with the grid. Check they still read comfortably at the sides, and that nothing is lost: the bookmark behind the triangle stays hidden, stacked icons stay whole.

Log: `galaxy map: the stencil write-backs ... are off`; `galaxy map: the two eyes' offsets ... are published`; the 30 s `map pointer:` line with stems swapped, eyes written, declined write-backs; `native timing CPU` frames in 10 s and transfer back near 0.2 ms.

## Flight card, fourth build (flown 13:21)

Right build first: `python tools\edvr_log.py --target <odyssey path> --expect-build HEAD --version`.

1. Pan the map sideways and look around. Expect: triangles, orange icons and rings' arcs line up with the stems and point at the grid wherever they are in the view; labels stay level. Look down toward the grid under you: no spinning. Insert dump.
2. Hover several stars with the mouse held still. Expect: the arrow holds its depth, with no jump. Then run onto a panel and off: one jump at the edge, as before.
3. Find a far icon behind a near triangle, as in the crop of 095453560001. Expect: the near one is whole, the far one hidden behind its solid parts and visible through its clear parts. Stacked icons at one star (a glyph on its base) stay whole, with no speckle, while the head moves and the map moves.

Log: `galaxy map: left eye's sprite depth created`; the 30 s `map pointer:` line with depth tests and issues, and panel draws for C4B4B3 and 5DAD41 only; flips of `the arrow's tip` no longer while hovering; `draw hook CPU` against about 1.2 ms before, for the cost of the second issue.

## Flight card, third build (flown 12:35)

Right build first: `python tools\edvr_log.py --target <odyssey path> --expect-build HEAD --version`.

1. Turn and tilt the head with the mouse still, looking at icons, labels, triangles and the rings' arcs. Expect: none of them moves in the world, and each is square-on when looked at. Insert dump. The dump's icon edges should now follow the head's roll (world level), not read 0.00.
2. Run the mouse slowly down the left rail, and across from the rail to the Trade Routes panel. Expect: the arrow stays at panel depth across the gaps, then just off the rail's outer edge it drops to star depth.
3. Pan and orbit fast, and look straight up the map's up axis if it can be reached: no pops.

Log: `galaxy map: the map camera's up is bound at vertex constant slot 10`; the 30 s `map pointer:` line with ring swaps, `noPose` at 0 and `camWrites` rising only while the map moves. `galaxy map: the arrow's tip` flips should be far fewer than the 16 in 4.2 s of the last flight. If these are missing, the code never ran.

## Sprite order (FLOWN in the fourth build, 13:21)

Sean, on the crop of dump 095453560001: "that orange bookmark is deep below the large green triangle and yet it appears drawn on top". The numbers agree: bookmark record 282 at 155 units, triangle records 513 and 514 at 6 units. In the enlarged crop the triangle's rim and fill show through the bookmark's dark veil and its orange is untinted, so the bookmark was painted after the triangle.

The game does sort. Dump 110152762001's list runs from 18470 units to about 10, and 2.8% of pairs are out of order by more than 5 units. The draw capture matched to dump 095453560001 has the bookmark at position 58 and the triangle at 315, which is the opposite of the image. The captures and the pool dump are not the same frame (the positions move frame to frame in `inst_*`), so that position is not evidence. OPEN: why this pair is painted out of depth order.

Built, not depending on the answer: a real depth test in the HDR layer for the map's sprites (Sean: "After this flight"). Each draw tests depth (GREATER_EQUAL, the game's depth being reversed) with no write, then a colourless second issue writes depth only where the sprite's alpha is at least 0.3, with a clip pixel shader and a bias of 4 steps so a later quad in the same plane is not lost to rounding. So a nearer solid part hides anything farther drawn after it, the see-through parts block nothing, and equal depths fall back to the game's order. Rings and stems test only. Everything beyond about 6500 units collapses to depth 0 in D16 and also falls back to the game's order.

Cost: a D16 target at the layer's size, 20 MB an eye at 4508x4510, let go when the map shuts, and a second issue per sprite. As first built it kept the stencil write-backs, on the reasoning that draws FC1193AFFC596F74 at the frame's start might read last frame's bit, and that halved the frame rate; see "The frame rate" for why they went. Left wrong: a far sprite after a near one still shows over the near one's soft edges.

## Flight card, second build (flown 11:00)

Right build first: `python tools\edvr_log.py --target <odyssey path> --expect-build HEAD --version`.

1. Yaw and pitch the head with the mouse still. Expect: orange icons and labels stay upright and undistorted across the view. The "10 : 88 : -100" labels and discs stay flat on the plane. Insert dump.
2. Move the arrow from the map onto a panel and back, slowly. Expect: the panels sit at their original depth with no clipping through the map; the arrow on a panel at the panel's depth, and off it over the map's icons, with one jump in depth at the edge.
3. Click a star near a panel's edge, then a side tab and the search box. Pan and orbit fast: icons must not pop.

Log: `galaxy map: the arrow's tip is on/off a panel` at each edge; the 30 s `map pointer:` line: draws swapped per shader, panel draws issued into the mask, probes made and answered, frames over and not over; `maskLate` at 0 (a panel draw after the arrow's probe would make the mask incomplete); `draw hook CPU` for the cost. If these are missing, the code never ran.

## Flight card, first build (flown 09:50)

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

### 2026-11-05, the System Map stands down with its return, BUILT, NOT FLOWN

`mapPointerFrame` turned the System Map's state on (`mapui::setSystemOpen`) from the camera alone, before it checked that the pointer was armed and that the build's System Map return (`s.systemReturn`) was found. So a build without the return, or a pointer that had stood down, still ran the marker and reticule swaps, the body table and the close view, although the log says the System Map stays as the game made it. One pure function, `systemMapOn` in `galaxy_map_pointer_core.h`, now decides it: the Galaxy Map is not open, the pointer is armed, the return is non-zero and a map is open. The same value feeds the gate, so there is one rule. The Galaxy Map's path is unchanged. The frame that arms the pointer reads the state off and the next frame reads it on, which costs nothing. `closeGate` still clears it, and nothing else sets it. Pinned by GP6d in `ui_quality_test`. Not flown.
### 2026-11-04, flight 3 (ca707034), the first build of the design

Sean: "the map is now crisp and doesn't smear under movement, you can select systems properly and still use the UI." Then: "The UI panel, now clips through the 3d space in depth"; "Yawing the head still causes the icons on the map to twist with the yaw."

Log: the hook armed, `GalaxyMapView` named, 13177 map publishes of which 2329 moved the map; `temporal aa map: 21606 eye-frames kept their history, 4852 reset it`; the pointer's call ran 1700 to 2250 times in each 30 s, all replaced, none passed through; b2 is 272 bytes, so the narrower tone admission was not needed but costs nothing. `draw hook CPU` 1.09 to 1.21 ms per sampled frame.

The self-check, first second: the game's point in the map's axes (-0.926, 0.954, 0.584) against P (-0.708, 0.471, 0.584): ratios 1.31, 2.03, 1.00. So the near pointer was anisotropic, not the 1.83 inferred from the fit, and the far point never rested on it.

### 2026-11-04, the build

Sean: "Let's build it", with panels at star depth. Changes against the first draft of this design, each found reading the code before building: the tone needs no table (VR admits by identity) but does need the bloom blend forced additive; P comes from the drawn scene object, not the controller; `SystemMapView` found in the exe. K's stores are `movups`, checked by disassembly, so the self-check's scratch output needs no alignment.

### 2026-11-04, flight 2 (7c1544a0), the reports this design answers

"The mouse cursor is not functional for selecting stars"; "yawing the headset rotates the icons around their axis"; "moving the headset causes different systems to be selected"; "moving the map itself causes lines and text to appear smeared or ghosted like they have not been put into the UI layer".

Log: `temporal aa map: 18636 eye-frames kept their history (at most 0.001 deg and 0.0490 off it), 1552 reset it (up to 161.846 deg and 69744.6940)`. The rotation bound was never close; the translation bound was reached. The probe opened (`map probe` line, F114b names the upright owner), but with no detail log there are no F111 cycle lines: the pool records gave the answer instead.

Rest figures from these dumps (16 frames each): rows' move 0 to 0.016, head translation 0.00002 to 0.0046. Head moving 0.45 to 1.9 degrees a frame in dump 1 and the rows follow within 0.002 degrees.

### 2026-11-04, flight 1 (948fa347), fix 2 of the gate

The no-jitter, no-history gate worked (map_frames=11164, every dump frame jitter 0 and reset) and text and icons shimmered at rest: each frame is a standalone upscale and the camera tremors about one input pixel a frame. In the history-on dump 193401951001 the pass's motion field was a clean rotation field, so history is right at rest. Built fix 2: jitter and history kept while the rows follow the head (flags bit 8, `temporalMapStillBits`). The pointer probe never opened on this flight: `temporalMapOpen(journalGuiFocus(&mapFocus), mapFocus)`, MSVC read mapFocus before the call wrote it. Fixed with a separate statement.

### 2026-11-03, flight 0 (d85658fc)

Dump 193401951001. The pass ran with jitter and history on the map; the map writes no depth (flat finding, docs\design-flat-temporal-aa-2026-09-23.md, 2026-10-10), so the pass has only the head's delta. Static RE of the cursor and the hover: `analysis\decomp\verify_20261103_galaxy_map_pointer.txt`, models M1 (lever), M2 (near sprite), M3 (fov scale). The dumps of 11-04 settle it: M1 out, M2 and M3 both present (near, and scaled).
