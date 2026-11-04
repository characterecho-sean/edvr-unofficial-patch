# VR Galaxy Map: motion and the cursor

## Status

State (2026-11-03): motion fix BUILT (full build.bat green), NOT FLOWN. Cursor: cause NOT found; observe-only probe BUILT, NOT FLOWN.

Evidence: Frontier eye dump stamp 193401951001 (log edvr_gfx_20261103_192955, v0.22.0-rc.6-19-gd85658fc, DLSS performance, 2016x2017 in, 4032x4034 out, preset K), Galaxy Map open (GuiFocus 6 in the 19:33:55 window).

Motion (built):
- The VR pass ran with jitter and history on the map. The map writes no depth (flat finding, docs\design-flat-temporal-aa-2026-09-23.md, 2026-10-10), so DLSS had no motion to resolve against.
- While Status.json GuiFocus is 6, 7 or 8 (temporalMapOpen, src\common\temporal_mode.h), native_temporal.cpp begin() sends the game no jitter and treat() hands the pass no history. The pass still upscales, so the HUD layer and the sharpen stay on. One more history-free frame follows the map. The Full System Scanner (9) is not included: its own latch owns it in VR.
- Known gaps: the journal lags up to 0.5 s, so the first frames after opening still jitter (flat has the same lag). On foot GuiFocus is absent, so the on-foot VR map gets no fix yet (flat infers it from the map canvas draw, FlatUiMapCanvasLatch; VR has nothing equivalent).
- Flight check: log `native temporal: map aa: no jitter and no history on the Galaxy Map (GuiFocus 6)`; at close `native temporal map aa: map_frames=N`; in an eye dump motion.csv jitter0/jitter1 = 0 and the pass's reset flag set on every frame of the map.

Cursor (open):
- Dump: right eye, tan x right / y up, the glowing hover ring is at (0.008, 0.176); the arrow sprite centre at (0.135, 0.072), hot spot (0.124, 0.088). Left eye ring (0.000, 0.180); the arrow is outside the 1400 px left crop in all 16 frames. Ring disparity is zero: the map is at infinity.
- RE (analysis-inbox 20261103-202226-galaxy-map-pointer, build 334925): the hover is a screen-space nearest-star test (0x3ADE6F0 over 0x3B91D80, GalaxyMapVisualisation 0x438 bytes), not a ray and not a Flash hit test. The arrow is the MouseCursor component (0x474A740, pose from 0x4505680 = controller +0x110..+0x14F), placed in the world from the pick camera C2 at the pick mesh's depth, plus a second matrix S * C2.proj * inverse(C1.proj) * C1.pose. INFERRED: the hover reads the sprite position (controller +0x140), so rewriting that position would move the hovered star too.
- Candidates, not separable by the one dump: M1 the on-foot rho lever (fits only with the pick axis 8 deg up; would invert mouse motion), M2 the pointer ray is right but the sprite sits near (about 0.56 m) while the stars are at infinity (needs the game camera 6.5 cm right of and 4.9 cm below the right eye), M3 the sprite lands on the UI-fov-scaled panel while the hover uses the camera fov (k = tan(0.5 x 1.0) / tan(0.5 x 1.571) = 0.546; predicts arrow (0.09, 0.096)).
- Discriminator: the sprite's tan per NDC unit. Camera tangent (1.065) = M2; UI tangent (about 0.58) = M3; negative = M1. Head translation moves the offset in M2 only.
- Instrument built: while GuiFocus 6, 7 or 8 aboard, native_on_foot.cpp f111RefreshGates opens the observe-only F111 pointer hooks and F114b's owner transform hook (no F154 placement, no look change). Log: `panel pointer (F111) map probe: ... open`, then the usual `panel pointer (F111) move:` and `1 s:` lines (detail log).
- Not found: the draw that consumes the cursor component's matrices, the projector K's class, the map panel's owner type (is it the 0x47299F0 screen-space owner, S+0x1C0 != 0?), System Map and Orrery hover.

Next flight: cockpit, Galaxy Map open, three Insert eye dumps with the cursor over three well-separated stars (wider left crop), detail log on; read the F111 lines against each dump's timestamp. If the map probe line never appears or there are no F111 cycles, the hooks did not open: widen the instrument (F154's owner hook for the owner type and S+0x1C0, and a hook on 0x3ADE6F0 to log K's projection of the pointer and the picked star).

Candidate fix (not built, wait for the flight): hook 0x4505680 and rewrite the pose copy's position after the original returns; that changes only the picture, not controller +0x140. Scaling the position also shrinks the sprite unless the size setter 0x1407B6DC0 or the rows are scaled too.

Ruled out: a symmetric-vs-asymmetric frustum mismatch alone (it gives a constant tan offset of 0.19 in x only; the observed offset has a y part of -0.104). Head-pose latency (head rows about identity in the dump; 0.13 tan is about 7.5 degrees).
