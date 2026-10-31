# SRV turret view: camera position and HUD depth

## Status

- **State (2026-10-31):** Sean's goal: stock camera; the arcs, ammo counts
  and INFO strip pushed out behind the location panel (Cranfield
  Nutrition); the radar near field on its own. FLOWN and settled: the 3D
  move with its anchor, the depth fix, the radar forward key, the draw-time
  pose (judder gone), the hologram's lighting position (45fa7e56: it draws).
  The radar disc is a picture shown by the hologram billboard vs 5559BD94
  + ps EA02FAC2, not DF3503CD. The instrument flight (707c0ab5) found that
  two writers draw it: the schematic vs 4EDE4BC9 (the disc) and vs
  5B0068AF into a 256x256 target (the Scorpion). Both draw from a FIXED
  camera V0 that does not follow the head: its constants were bit-identical
  16 s apart while the head moved 0.28 m. At camera back 25 the whole radar
  volume lies 0.36 to 0.51 m BEHIND V0, so the picture is empty and no
  billboard move can show it. V0 and its window are chosen on the CPU (the
  window is framed for a camera 0.65 m behind V0). NO GPU-side fix built
  (journal, "the radar's camera is fixed"). Branch `claude/srv-turret-hud`,
  local only. Issue #140. Test install: Steam (`--target steam --scope
  both --xess`).
- **The radar's way out:** camera back 70, radar forward 0. Dump
  130228598002 shows the whole disc, rim and Scorpion, as the game draws it.
  A GPU-side fix would have to replace V0 and its window with our own, and
  rebuild the CPU-made billboard quad. It is not known when V0 updates, and
  the CPU cannot tell the radar's 5B0068AF draw from the others.
- **What is wrong (the game's own placement):** the turret view's whole HUD
  sits about 0.7 m too far back for the stock camera (the radar behind the
  head, the arcs at or behind it), and the larger panels sit about a metre
  deeper than the arcs, ammo counts and radar.
- **What the pass does:** in the Scorpion's turret view only, a private
  geometry shader per output signature (eight) rebuilds each vertex of nine
  HUD families in the eye's space, moves the panels by HUD forward and the
  radar by radar forward along the seat, pushes vertices within 1 m of a
  latched anchor (the settled head) out by the group's back key, and
  projects again. Camera back moves the real camera.
- **Not covered:** Elite's "Disable GUI effects" OFF draws the holo panels
  with vs 81216C77F90DEDD6 (another output signature, no bytecode yet).
- **Temporary keys** (live, Fixes page of the menu; bake the flown values and
  propose removing them when the arc closes): `fix.srv_turret_camera_back`,
  `fix.srv_turret_hud_forward`, `fix.srv_turret_radar_forward` (70),
  `fix.srv_turret_panels_back`, `fix.srv_turret_radar_back`,
  `fix.srv_turret_hud_gap`, `fix.srv_turret_hud_depth` (front),
  `fix.srv_turret_hud_scissor` (open). Sean's flown values
  (2026-10-31): camera back 35, HUD forward 70, panels back 140, radar 0.
- **Latency:** a view switch is followed up to about 0.5 s after the game
  writes Status.json (the watcher's 500 ms poll); left alone, a fix would
  change on foot too (journal).
- **Open:** whether camera back is still wanted once the anchor is sound
  (Sean credited it, but its change also fixed the anchor); where the radar
  reads best; whether the switch lag is acceptable.
- **Ruled out:** see "Ruled out" below.
- **Next flight:** camera back 70, radar forward 0; tune HUD forward and
  panels back for the arcs and panels at that camera. No new build needed.

## Evidence (flight of 2026-10-30, Epic, v0.22.0-rc.3-139-gcd4c91c4)

Log `edvr_gfx_20261030_124737.log`, eye dumps `125141609001` and
`125159159002` (manual, 18:51:41 and 18:51:59 UTC, seat kind 0x4). Pimax
Crystal Super, Pimax OpenXR, 4032x4034 per eye out, 2016x2017 render,
DLSS. Game FOV radians L -0.89775/0.71858, R -0.71858/0.89775 (left/right),
world scale 110 %, so Elite's eye separation is 55.1 mm.

**Depth from the two eye overviews.** Each region of the left overview is
matched to the right by normalised cross-correlation (an orange mask for HUD
pixels, luma for the world). Pixel x converts to a tangent through the
eye's own FOV, and depth = 55.1 mm / (tanL - tanR).

| Region | Depth |
|---|---|
| Far ridge (calibration) | infinity (dtan +0.0004) |
| Reticle | infinity, at the target |
| Target info panel | 1.176 m |
| Gun barrel | 0.347 m |
| Weapon arc | 0.158 m |
| "SECONDARY A" | 0.143 m |
| Ammo count | 0.161 m |

The ridge at zero tangent difference shows the two views are parallel and
the method's sign and scale are right. The runtime's `seat_meter` lines at
18:51:22 and 18:51:52 put the head 0.25-0.28 m behind the seat origin, with
no recentre: the arc layer is about 0.11 m behind the default eye point,
the info panel about 0.91 m in front of it.

**Which draws.** The dump's draw state (`tools\eye_draw_snapshot.py
--surfaces`) exports the two holo panel surfaces drawn by vs
1989E6D3B405FDE0: the 2005x1253 one holds both arc brackets, both weapon
names and both ammo counts (the near layer); the 3008x646 one holds the INFO
strip. (First read here, and WRONG: that the info panel, the SYSTEMS panel
and the reticle were the three draws of vs E508648660A352B2. They are the
reticle's three layers at about 500 m; see the journal entry below. The
deep panels are records of the same two holo families about 1.39 m out.)
Both holo shaders build a camera-relative position and project it with
`dp4 cb0[4..7]` (the rows' translation is zero), so clip w is the depth
along the eye's forward axis.

**Why not a camera move alone.** Both layers are fixed in the turret frame
about 1.02 m apart along the view, and moving the camera moves both by the
same amount. Matching them needs a change to one layer's stereo depth.

## Ruled out

- ruled out: an EDVR path placing the HUD at its depth (UI layer, HDR HUD
  layer, `fix.ui_depth`, the hologram remap), because none of them changes
  a vertex position: the remap edits an SV_Position address in a pixel
  shader, the layer and ui_depth redirect or redraw the game's own draw, and
  the measured depths agree with the game's geometry (ridge at infinity,
  barrel at 0.35 m).
- ruled out: world scale (110 %) as the cause, because it scales every
  eye-pair disparity alike and cannot make a 7:1 depth ratio between two HUD
  layers.
- ruled out: seat memory moving the turret camera, because the flight's
  runtime log has no `seat_reset` between boarding and the dumps; the head
  sat 0.27 m back by Sean's own lean.
- ruled out: the targeting sprites (vs E508648660A352B2) as the info and
  SYSTEMS panels, because a CPU emulation of that vertex shader on the eye
  run's own inputs (145941154001) puts all three at clip w of about 500 m
  (world scales 122, 94 and 47 m), and its NDC centre is the reticle's place
  in the image: they are the reticle's three layers at the target. Flight
  2026-10-30 14:59 (gap 102 cm, bound on every draw) agrees: no panel moved.
- ruled out, for the shift that had no effect: the HDR HUD seed clearing the
  geometry stage (its command list runs with RestoreContextState TRUE); the
  door's hologram resolve (saves and restores the stage around its own
  draw); the shader linkage (ps 63ABD86359B57D01 reads TEXCOORD0 only); an
  issue of the sprite outside the bound scope (traced, none).
- ruled out: the radar families as the deeper panels, because the flight of
  2026-10-31 (9fdad3a1) captured DF3503CD07F9B10C, 44DDCDEBAEA137D8,
  9B34C331902DC1ED, 9611A454527F7FEB, B932058F26B76691 and the near draws of
  5559BD94.EA02FAC2 at clip w 0.11-0.44 m, bottom centre: they are the radar,
  nearer than the arcs, and a forward pull can never reach them.
- ruled out: vs B7790CBFC6554097 as a HUD panel, because its captured
  vertices sit at w -658 to +3029 m.
- ruled out: a stale scissor rectangle cutting the radar, because the
  state lines of flight 9f98e10f read scissor off for every turret family,
  and Sean saw no difference between `open` and `game`.
- ruled out: the game turning the radar disc toward its camera (a
  billboard), because the disc plane instrument held its seated normal
  (0, 0.992, 0.130) to 0.0 deg and its centre to 0 mm while the head moved
  0.86 m. (Measured on DF3503CD, which is not the disc Sean sees; the
  visible disc is the 5559BD94 billboard's picture, below.)
- ruled out: an unmoved copy of the radar from another path, because the
  only unmoved family draws were with the radar group's keys at 0.
- ruled out: a disparity-only push as the fix, because it changes only the
  eyes' horizontal disparity: the panel keeps the motion parallax and the
  growth on approach of its true 0.5 m while stereo puts it at 1.6 m. Flight
  2026-10-31 06:07 (e45c68a9), Sean: the radar and panel "move with my
  head", and the panel "got larger" as the camera moved towards it.
- ruled out: DF3503CD as the disc Sean sees, because it adds colour
  (SRC_ALPHA, ONE) and keeps the layer's alpha (ZERO, ONE), so it cannot
  darken the eye, and the layer's difference image at dump 110805769001
  holds nothing over its moved quad. The disc probe and the disc plane
  measured an invisible draw (journal, "the radar disc is a picture").
- ruled out: the radar's picture being drawn from the head, because its two
  writers (vs 4EDE4BC9, vs 5B0068AF into 256x256) carried bit-identical
  camera constants at 13:01:40 and 13:01:56 while the live camera moved
  0.28 m and turned (flight 707c0ab5).
- ruled out: the game cropping the radar's writers to the visible sliver of
  the radar volume, because their windows aim 48 deg below V0's forward at a
  point 0.215 m ahead of V0, while the whole schematic box lies 0.36 to
  0.51 m behind V0. Writer #6, the 44-deg-down sliver, is not the radar's:
  it uses the live camera and its billboard sat 2.2 to 2.6 m out.
- ruled out: a billboard move showing the disc at camera back 25, because
  the picture is empty there (the volume is behind V0); dump 130153201001
  has no disc at all.
- ruled out: placement as the cause of the radar's cut and stretch, because
  camera back 25 + radar forward 45 (sum 70, flight 82546e51) still cuts it:
  a moved billboard shows a picture taken from the unmoved place.
- ruled out: one forward pull making the layers meet, because a pull only
  moves vertices deeper than gap + 0.1 m. The near layer (0.11-0.70 m) is
  never in the window, and the deep layer itself spans 1.3-2.8 m.

## Journal

### 2026-10-30: diagnosis and the build

Sean's direction after seeing the measurements: "move the starting position
back so the camera exists above the central axis of the turret and then
bring the deeper UI layer forward to be matched with the ammo count". The
camera default (60 cm) comes from Sean's own screenshot of the recentred
turret HUD: the ratio of the info panel's width to the arc's height there
(0.5, against 0.185 in the dump) puts that camera about 0.68 m behind the
default eye point, the arc about 0.57 m away. With 60 cm the arc lands near
0.49 m; the gap (102 cm) brings the panels to the same depth.

### 2026-10-30: two Steam flights, and the panels found offline

Flight 1 (d662d26c, 14:20): camera back 60-65 cm, "pretty good". The gap
key swept 102 to 0 twice with no visible change. Dump 142608727001 (gap 0):
near layer 0.44-0.47 m, info panel 1.31 m, SYSTEMS 1.54 m, reticle at the
target. Dump 145941154001 (gap 102): 0.41-0.43 m, 1.29 m, 1.41 m: unmoved.

Offline, from that eye run (Sean asked for RE instead of another flight):
the draw-state copy holds each sprite draw's b0/b1/b2, its packed vertices,
the per-instance index and the t33 pool, enough to run vs E508648660A352B2
on the CPU. All three sprites are the reticle at about 500 m (see Ruled
out). The ledger (`draws_<stamp>.bin`, `inst_`, `pool_`) then gives every
instanced draw's record translation; along the eye's forward axis, in the
left eye's HDR target: 1989E6D3B405FDE0 at 0.42, 0.41 (arcs surface), 0.44
(INFO strip), 0.41 and 1.39 m; 2CECEC3065EF0D4A at 0.41, 0.41 and 1.39 m.
The deep panels are those two families' records about 1.39 m out, drawn by
the same shaders as the near layer, so the shift is per vertex by depth.
Default gap 96 cm = 1.39 - 0.42.

### 2026-10-31: the ten-family pull flown, and the push-back plan

Flight `edvr_gfx_20261031_051457.log` (9fdad3a1, Steam, build matches). The
latch read the Scorpion from LoadGame and cleared on DockSRV. Turret view
236 s, camera back 70 cm; Sean swept the gap 0 to 200 cm and back. The pass
bound every draw of the ten families (121066 of 121066). Sean: "the panels
still exist off set in space from one another regardless of the srv
settings".

The stream-output captures show why, per family, left eye, across the
sweep: the arcs, weapon names, ammo counts and INFO strip (1989E6D3 draws
0-3, 2CECEC30 draws 0-1) at w 0.27-0.62 m; the radar (the five
`__USER_RADAR_LOCALKEY` families and the near 5559BD94.EA02FAC2 draws) at
0.11-0.44 m, bottom centre; the deep draws 1989E6D3 draw 4 and 2CECEC30
draw 2 at 1.32-1.50 m; 5559BD94 (both signatures) deep draws at 1.77-2.78 m
lower centre. A forward pull only moves what lies deeper than gap + 0.1 m,
so the near layer never moves, and the deep layer's own 1.3-2.8 m spread
stays. Dump 052114218001 (gap 0) gives the depths in the Status block.

Sean's plan from this: "move the radar and weapon panels further away from
the center of the turret to be in the same z plane as the larger UI". The
same shader does that with a negative gap: a vertex at w goes to w + d along
the head-centre ray at unchanged angular size (x += -s d / (w + d)). Because
stereo disparity goes as 1/distance, a push also shrinks the remaining
spread. At d = 110 cm the arcs (0.42-0.65 m) land at 1.52-1.75 m, the radar
at 1.42 m and SYSTEMS stays at 1.70 m. The vergence difference between
SYSTEMS and the nearest arc falls from 1.79 to 0.07 dioptres (1/0.42 -
1/1.70, against 1/1.52 - 1/1.70). The cost is that the radar lies on the
turret deck (0.30 m) and the ammo counts cross the barrel (0.62 m). Pushed
behind them, they are still drawn over the metal.

### 2026-10-31: the push-back flown; it follows the head

Flight `edvr_gfx_20261031_060739.log` (e45c68a9, Steam, build matches). The
pass bound every draw of the nine families (372878 of 372878). Sean swept
panels back 95-200 cm, radar back 65-170 cm and camera back 15-70 cm. Dump
061223993001 (camera back 70, panels back 110, radar back 170, head centred)
measures, by disparity: SECONDARY A and PRIMARY A 1.60 m, the location panel
1.46-1.48 m, SYSTEMS 1.58 m, the INFO strip 1.62 m and the radar 1.83 m. In
stereo the layers now meet.

Sean: "the radar and panel seems to move with my head, also the panel got
larger if I move my camera position towards it". This is the method itself.
The shader moves each vertex along the ray from the head centre of the
current frame and changes only the eyes' horizontal disparity, so the panel
keeps the game's 0.5 m geometry for everything else. Moving the head 10 cm
towards it grows it by about 25 % (0.5 / 0.4), where a panel truly at 1.6 m
would grow by 6 %. Moving the head sideways, it slides across the scene as a
0.5 m object would, against a stereo depth of 1.6 m. The location and SYSTEMS
panels are truly at 1.5 m, so the difference shows beside them.

The next build moves the panel in 3D. Each near vertex goes along the ray
from a FIXED anchor in the seated frame, not the head. The anchor is the head
position when the turret view comes on, latched again on a recentre. From
the anchor the panel looks exactly as it does now. Away from it, the panel
has the parallax, the growth and the disparity of a panel at 1.6 m. The
geometry shader rebuilds each vertex's eye-space point from clip x, y and w
through the eye's tangents. It moves the point and projects it again, with
the new w and the same NDC depth. Per eye per frame it needs the anchor in
that eye's space, from the head pose and eye-to-head transforms the host
gives the temporal pass (native_temporal.cpp, `f->head`, `f->eyeToHead`).
NDC y is up: the radar's captured ndc_y is -0.5 to -1.6, and it sits at
the bottom.

### 2026-10-31: stock camera, HUD forward, the 3D move flown

Sean's direction before the build: stock camera; the ammo-count panel
pushed out behind the location panel; the radar near field, separate. At
the stock camera the radar sits behind the head, so b5c49237 also moves
the whole HUD forward along the seat (`fix.srv_turret_hud_forward`).

Flight `edvr_gfx_20261031_070525.log` (b5c49237, Steam, build matches),
turret view 126 s; the pass bound 348772 of 348806 draws. Sean: "kind of
what I want, pushing the camera back seems to put the panel in the right
relative space, but it also seems to try and stretch toward my camera".

Why it stretched. The view came on at 07:07:51. The anchor latched ("view
on") at -0.415,-0.366,-0.682 m seated. The headset was not tracking: the
runtime's seat_meter gave the bit-identical head_local -0.543/-0.000/-1.138
at launch (13:05:27 UTC) and at 13:07:36, and a live 0.135/0.365/-0.575 at
13:08:06. From 07:07:56 to 07:08:36 the 10 s lines read head_offset 88-112
cm: the panel was pushed along rays from a point a metre from the head. The
20 cm jump rule never fired when the pose went live. The camera-back change
at 07:08:40 re-latched next to the head (head_offset 2.3 cm, then 0.1-1.5
cm), which is when Sean saw the panel "in the right relative space".

Dump 070908275001 (good anchor; camera back 35, HUD forward 70, panels
back 140, radar 0): SECONDARY A 1.65 m, PRIMARY A 1.96 m, INFO strip
2.02 m, location panel 1.14 m, SYSTEMS 1.27 m, reticle at the target, the
radar's rim about 0.33 m (weak match). The arcs panel sits behind the
location panel, as Sean asked.

cd67ee63: a settle frame counts only when the head moved, so a frozen pose
never latches; a latched anchor more than 0.35 m from the head for 90
frames is taken again ("head away").

### 2026-10-31: the anchor fix flown; depth clipping found offline

Flight 5ed1c405 (Steam, build matches), dump 073113451001; camera back 35,
HUD forward 70, panels back 140, radar 0. The anchor held (head_offset 2.6
cm). Sean: "basically the right spot, but if I move my head it clips some
of the display, also the radar is distorted, not really visible".

The dump's draw state gives the hologram families' cb0[6] = (0, 0, 0,
0.025): infinite reversed Z with a 2.5 cm near plane, so NDC z = 0.025 / w.
The shader kept each vertex's original NDC z. A vertex the game put behind
the eye (w < 0), or inside the near plane, therefore had a depth outside
[0, 1] after the move, and the rasterizer's depth clip cut it. The capture
lines at 07:31:10 put the radar 0.21-0.32 m from the anchor before the
move, at an original w of about -0.2 to +0.03, behind or at the eye: nearly
all of it was cut. The panel, about 0.2 m ahead at camera back 35, is cut
as soon as a lean takes its original place behind the eye. A second fault:
a move ending behind the eye returned the UNMOVED vertex, so a triangle
could mix moved and unmoved corners.

9330af6e: the game's own NDC z where it is valid, else 0.999 (the nearest
valid reversed-Z depth); every corner moved, the rasterizer clipping what
is truly behind the eye; the capture line counts the fallbacks (`zfix`).

### 2026-10-31: judder and the radar disc (built, not flown)

Two faults after the depth fix: the HUD judders as the head moves, and the
radar disc (DF3503CD), carried forward with the HUD, is level with the eye
and reads as a sliver.

Hypothesis 1 (judder): the HUD follows the head one frame late. The frame
tick (cacheGeometry) runs at the Present boundary; the host's begin()
(native_temporal.cpp, at WaitGetPoses) publishes the pose. If begin() runs
after the tick, the tick built both eyes' frames from the previous frame's
pose and frame N's draws used frame N-1's. The new counter decides it:
`pose refreshed at draw: N of M frames` in the 10 s line is the frames
whose draw found a newer frame sequence than the tick built from. N near
M: the pose WAS a frame old. N near 0: begin() runs before the tick in
this game and the hypothesis is dead. The code alone cannot say, since the
order of Present and WaitGetPoses is the game's.
Change: begin() publishes the frame sequence (nativeTemporalFrameSequence);
the draw (drawEyeFrame) rebuilds both eyes' frames once for each new
sequence from that frame's pose and the tick's anchor.

Hypothesis 2 (radar): the HUD-forward translation carries the disc level with
the eye, so it is seen edge-on.
Change: `fix.srv_turret_radar_forward` (TEMPORARY, 70, 0..150) moves the
radar group by its own distance; the panels keep HUD forward. The 10 s line
prints `T=` and `Tradar=`. Fly radar forward 30 and 70 and read the disc.

### 2026-10-31: the Scorpion hologram and the radar disc (built, not flown)

Flight 3411dc8e (Steam, dump 083539722001): the pose counter read 900 of 900
frames, so the stale pose was real and is fixed. Sean: "almost perfect, we're
just missing the hologram of the scorpion and the radar disc."

The hologram is vs 5559BD94B6852E83 with ps EA02FAC2BD6C643C, its near quad
(draws 1-2). Its vertex shader writes o0 = M_LIGHTINGPOSITION = cb0[9..11] *
pos, the camera-relative WORLD position, and its pixel shader uses that twice:
a scene depth test (dp3 with cb1[279], the fragment's view depth) and a view
ray (v0.xyz divided by that depth). The geometry shader moved SV_Position but
not v0, so a quad the game had beside or behind the eye kept a view depth near
0 or negative, the ray blew up and the hologram vanished. cb1[277], [278] and
[279] of the draw's vertex constants are the camera's right, up and forward
unit vectors in world axes (orthonormal, right x up = forward, matching the
projection rows). Fix: for signature H the module binds the draw's VS slot 1
to GS slot 1 (with the range the game bound it with), and the shader adds
right * dX + up * dY + forward * dZ of the move's eye-space displacement to
the interpolant. Not instrumented: the dot with cb1[279] before and after
(`v0z`), since the twin streams out the clip position before the move but not
the interpolant.

The radar disc is vs DF3503CD07F9B10C with ps 8C08EB252B0F6095. In the flight
it is rasterized (ps about 2.2 M per issue; capture at w 0.24-0.45 m, zfix 6)
and the final image shows nothing there; the whole HUD reaches the eye through
the crisp take's HDR HUD layer ("redirected into the layer"), and the
before-UI image has no HUD. The radar icons (all at the 0.999 fallback) show;
the disc, with half its vertices on the game's own depth, does not.
Hypotheses: (1) a depth or stencil test rejects it in the layer; (2) the game
faded it (cb2[11].w or cb1[90].y about 0). One flight decides: the first
scope of each family logs the state of every issue (`srv turret hud state:`,
target game or other, depth-stencil, blend); the disc's sampled scope adds an
occlusion query and a copy of the two registers (`srv turret hud disc:
samples_passed, alpha, intensity`). samples_passed 0 with ps running = (1);
samples above 0 with alpha about 0 = (2). And `fix.srv_turret_hud_depth`
(game, real, front) writes the moved depth three ways: if `front` or `real`
shows the disc, the depth the shader gave a vertex that was on the game's own
depth was the cause. Compare the disc's state lines with 9B34C331's.

### 2026-10-31: the scissor (built, not flown)

Flight 45fa7e56 (Steam, 09:56, dump 100017385001). The Scorpion hologram
(5559BD94.EA02FAC2 near quads) now draws: the lighting-position fix works.
With hud depth `front` Sean: "it renders in the correct position, but it's
still being clipped and distorted. It also moves with my head."

Disc probe: samples_passed about 3.9 M, alpha 1.0, intensity 0.0039. State
lines: the radar families draw with depth and stencil OFF; only 1989E6D3 and
2CECEC30 depth-test (func 7, no write). In the dump the disc is a dark
translucent ellipse with a yellow rim, but only its upper half draws: its
lower edge is a straight horizontal line, the dark tint stops on that line,
and the hologram's outline is cut by a straight horizontal line too. It is
not near-plane clipping: the 10:00:17 capture has every DF3503CD vertex at w
0.19-0.39 m after the move, and the quads span ndc x -0.17..0.76, y
-0.48..+0.06, far more than the visible dome (x 0.10..0.38, y -0.07..+0.05).

Hypothesis (untested): the game sets a SCISSOR rectangle for the radar's
draws, computed on the CPU from the radar's original screen bounds (around
and behind the head at the stock camera). The moved geometry is cropped to
that stale rectangle, and the rectangle shifts as the head moves, so the
radar appears to move with the head. What would refute it: the state lines'
new raster fields read scissor=0 for the radar families; or, with the key at
`open`, the line is still there.

Change: the state line gains the rasterizer state (ScissorEnable, rect count
and first rect, DepthClipEnable, CullMode, FrontCounterClockwise) and the
first viewport. `fix.srv_turret_hud_scissor` (TEMPORARY, live): `open` (the
default) sets every scissor rectangle of a bound draw to the whole render
target (RTV 0's size, else the viewport's extent) while it is issued, when the
bound rasterizer state has ScissorEnable, and puts the game's back exactly
after; the rasterizer state object is not touched. `game` keeps the game's
rectangle. The 10 s line counts `scissor opened N`; each family found with
the test on is logged once. `fix.srv_turret_hud_depth` now defaults to
`front`. Not done per issue: the rectangles are opened once per scope,
after the bind, so an issue that sets its own scissor would not be covered;
the state lines (taken after the open) show what each issue ran with.

### 2026-10-31: the disc's shape and a second copy (instruments, built, not flown)

Two more suspects for the radar before the next flight; each has an
instrument and neither a fix.

Billboard. Sean asks whether the disc will read as a proper oval once the
scissor is open. The dumps disagree with a turret-fixed horizontal disc:
camera back 70 is a flat ellipse on the deck, HUD forward 70 a near-vertical
sliver, radar forward 30 a near-circular dome. If vs DF3503CD ORIENTS the
disc toward its camera or the head (clip = cb2[9] + h*cb2[7] + u*cb2[6] +
v*cb2[8]), our rigid translation keeps an orientation meant for a viewer
elsewhere, so it looks stretched and swivels with the head. Instrument:
once a second, for eye 0's sampled DF3503CD scope, the twin's PRE-move clip
positions give each quad (six records) in the seated frame (eye-space point
from the eye's tangents, then E + R (X, Y, -Z), the inverse of eyeFrameFrom's
step for the anchor): `srv turret hud disc plane: inst=K centre=.. normal=..
head=..`. A turret-fixed disc keeps centre and normal as the head moves; a
billboard's normal turns. `edvr_log --health` gives each instance's largest
angle between normals, how far the head and centre moved, and says which it
reads as (head moved under 5 cm: nothing to tell). With two issues in a
scope the later quads repeat the first issue's.

Second copy. Sean: the radar "seems to be stretched toward its origin". If
some path issues the radar's draws OUTSIDE the scope, an unmoved copy lands
at the original place around and behind the head and smears toward it.
Instrument: every draw of a family that reaches the GPU without the pass's
shader bound is counted per family and route and listed in the 10 s line
(`unmoved family draws: F route N, ...` or `none`), with a "first unmoved
family draw by route" line for each route. Routes seen: `refused` and
`group-off` (the scope declined it), `fast-path` (the owner's thunk skipped
the scope), `indirect` and `auto` (those thunks have no scope), `foreign-
immediate` and `foreign-deferred` (a second context; the family is read from
that context's own shaders), `reissue` (pureDrawReissue called with the
shader not bound, with its call site as module+offset), `command-list` (the
game executed a list: the draws inside cannot be seen, so the family is
`?`). Read from the code, not from a flight: EDVR's own command lists
(ui_layer.cpp: the depth-stencil seed, the glow feed, the crisp coverage)
draw EDVR's own shaders and are replayed through vScreenExecuteCommandListRaw,
below the hooks, so they are neither counted nor families; the HDR HUD seed
is one of them, not a replay of the game's radar draws. A game command list
is seen only by its execution; a draw recorded on a deferred context is
seen at record time; vscreen.cpp's own note says the game's ExecuteCommandList
has never been seen on this game, so a command-list count would be news. Not
seen: anything that bypasses the hooked vtable.

### 2026-10-31: the radar flown with both instruments; main merged

Flight 9f98e10f (Steam, build matches). Sean: radar forward "causes it to
stretch, getting more elongated in the z axis and eventually clipping"; no
difference between scissor `open` and `game`. The state lines read scissor
off for every family: the scissor is ruled out. The disc plane held its
seated normal (0, 0.992, 0.130) and centre (-0.008, -0.215, 0.424) to 0.0
deg and 0 mm over 349 samples while the head moved 0.86 m: the disc is
turret-fixed, not a billboard. The only unmoved family draws were with the
radar group's keys at 0 (the group off, and the holo path's re-issue of
those same draws).

So the stretch is where the disc is. It lies nearly flat 21.5 cm below the
seat's eye height and 42 cm behind the seated origin. With camera back C
and radar forward F it sits (C + F - 0.42) m ahead of a head at the origin.
At C = 70, F = 0 (the dump 052114, a clean ellipse on the deck) that is
0.28 m ahead and 0.21 m below, seen from 37 deg above. In this flight C = 25
and F = 15 to 40: from 0.17 m behind to 0.23 m ahead, under the chin, cut
by the near plane and seen hugely foreshortened. Radar forward plus camera
back of about 70 cm gives the game's own view of the radar.

Main (4b2c0a04, 47 commits) merged into the branch as c12b0beb at Sean's
request. One conflict, native_on_foot.cpp's alignment block: the turret's
camera term and main's F186 ADS turn, both kept.

### 2026-10-31: the radar disc is a picture (instrument, built, not flown)

Flight 82546e51 (Steam, build matches; camera back 25, radar forward 45,
depth front; dump 110805769001 at 11:08:05). The sum is 70 and the disc is
still the top of an ellipse with a straight lower edge. Read offline:

- **DF3503CD is not what Sean sees.** Its blend adds colour (SRC_ALPHA,
  ONE) and keeps the layer's alpha (ZERO, ONE). The layer composite is
  eye x alpha + colour, so it cannot darken the eye, and the disc's fill
  darkens it by about 100 of 255. The final image minus the pre-composite
  image of the same frame (13253, `FinalCrisp_LOverview` minus `T00`) shows
  the layer changed only the dome, the Scorpion's fragments and a few
  contact pixels. Nothing lies over DF3503CD's moved quad (11:08:06: ndc x
  -0.03..0.59, y -0.32..0.12) though it shades about 0.8 to 1.1 M pixels a
  sample. Only the dome's apex (0.29, 0.11) touches the quad's far edge.
- **The visible disc is the 5559BD94 + EA02FAC2 billboard.** At the whole
  disc of 10:47 (dump 104729573001) the visible ellipse spans ndc x
  0.069..0.329 (0.26 wide), y -0.31..-0.03: taller than wide. DF3503CD's quad
  that second was 0.47 wide (x -0.065..0.401), too wide for that ellipse.
  The EA02FAC2 draw 1 that second was a camera-facing square (w 0.462 at
  all four corners, x -0.014..0.430, y -0.408..0.036) that holds it. Its
  blend (ONE, INV_SRC_ALPHA; alpha ZERO, INV_SRC_ALPHA) is the kind that
  darkens.
- **The billboard shows a picture.** ps EA02FAC2 reads t4 at the
  billboard's own UV: the fill where t4 is under 1000, the yellow outline
  from an edge filter over t4 (weights 0.09375, 0.3125), the colours from an
  ID in t5 (cb2 rows 2..9), more from t6. The disc's fill, its rim and the
  Scorpion's outline are drawn into t4 and t5 by another pass, from the
  game's camera. Moving the billboard by T shows that picture, of the disc
  at its unmoved place under the chin and cut by that pass's near plane, at
  the moved place. The straight edge carries no outline, so it is not an
  edge inside t4. It is where the picture or the billboard ends.
- It fits every dump. The disc is whole when the game's own disc was wholly
  in front of its eye: 10:47 had zfix 0 (yet taller than true: the picture
  is from 0.3 m nearer), and 05:21 (C 70, F 0) left the billboard unmoved.
  It is cut when the game's disc was not (zfix 6 to 12 at 08:35, 10:00 and
  11:08). The picture is re-made every frame from the head: "moves with my
  head".

Instrument (srv_turret.cpp, "the hologram's source"; TEMPORARY): a round
every 15 s while the pass is wanted. (1) One frame's 5559BD94 draws log
what they read at t0..t7. (2) The render and depth targets among t2..t7
are watched. (3) In the next frame every owner-context draw with one bound
as a target is logged as a writer: shaders, draw, targets, viewport. (4)
The b0 rows 0..15, b1 rows 270..279 and b2 rows 0..15 of six billboards and
ten writers are read back. The near billboard's six vertices (pre and post
clip, UV) are logged once a sample (`srv turret hud holo quad`). Discriminating:
writers found, with a view-projection in their constants, give the fix
(the camera at E - T through the billboard's original window); zero
writers point at a compute pass, a copy or an indirect draw.

Moved here from the Status block (to keep it short), the latency note: both
parts follow Status.json, which the watcher polls every 500 ms and the game
rewrites about once a second. The on-foot eager poll
(`journalWatchSetEagerStatusOnFoot`) never shortens the worker's sleep:
`untilNextDue()` honours `g_eager` only. Left alone; a fix would change on
foot too.

### 2026-10-31: the radar's camera is fixed (flight 707c0ab5; no fix built)

Steam, log `edvr_gfx_20261031_125952.log`, build matches. Keys at view-on:
camera back 25, HUD forward 65, radar forward 45. Dump 130153201001 (C 25,
F 45) has no disc at all, only a yellow tick. Dump 130228598002 (C 70, HUD
forward 0, radar forward 0) has the whole disc, rim and Scorpion. Holo
rounds 1 (frame 11896) and 2 (13039) ran; rounds 3 to 5 found no 5559BD94
draw because the radar group's keys were 0. Round 1's writer-to-billboard
map is exact (the 3-target sets match). In round 2 the game's 512x512
hologram targets had changed hands between the two frames, so its map is
not. Shaders from the dumps (`edvr_logs\shaders`, read with fxc /dumpbin):

- **vs 5B0068AF5630F96B** is the instanced mesh shader: X = the t33
  record's world position (bytes 16..27) - cb1[275] + the record's
  rotation and scale applied to the unpacked vertex. Then clip = X.x cb1[270]
  + X.y cb1[271] + X.z cb1[272] + cb1[273], w = cb1[274].X. It declares
  CB1[276]: only rows 270..275. o1 = X. **ps 6C416587** writes t4 =
  cb1[279].X (the view depth) plus a rim term; one target.
- **vs 4EDE4BC9003BE352** is the radar's terrain schematic (345600
  vertices). clip = cb0[4..7] times the local vertex. The local vertex is
  clamped to a cylinder of radius cb2[0].w = 35.5 about cb2[0].xz and capped
  at 0.6 r above. **ps 20D43145** discards below -0.8 r, writes t4 = 1e10
  beyond r, else the clip w; three targets.
- cb0[9..11] of the 4EDE draw is local to camera-relative world: orthonormal
  columns times 0.002, so the 35.5 radius is 7.1 cm. cb0[4..7] = VP x that,
  so VP = M A^-1 comes out exactly; its w row is the camera's forward, and
  its x and y rows give the window:
  - its camera's right (-0.38104, 0.11083, -0.91789), up (-0.23711, 0.94787,
    0.21287) and forward (0.89364, 0.29875, -0.3349) are those of the 256x256
    5B0068AF writers' cb1[277..279] (#9 round 1, #11 round 2);
  - they are not the live camera's (#6: forward (0.90856, 0.23581, -0.34483)
    in round 1, (0.91733, 0.1654, -0.36216) in round 2).
  So the radar's two writers, the schematic (the disc) and the 256x256
  101961-vertex mesh (the Scorpion icon), share one camera V0. Eye 1 is V0
  plus 61.9 mm.
- **V0 does not follow the head.** The 4EDE constants of round 1 and round 2
  are bit-identical, and so are the 256x256 writers' camera rows, while the
  live camera moved from (-0.118, 2.202, 0.268) to (-0.365, 2.125, 0.390). In
  V0's frame the schematic's centre is at (-0.007, -0.241, -0.437) (x
  right, y up, z forward), and the cylinder's box spans z -0.513 to -0.359.
  The whole radar volume lies behind V0, so the writer draws nothing and
  the picture is empty at camera back 25. In round 2 the live eye was
  0.27 m behind V0, which puts the schematic about 0.17 m behind the live
  eye. That agrees with DF3503CD's pre-move w (-0.23 to -0.03) that second.
- **The window is framed for another camera.** The schematic's window is
  tan x -0.145..0.147, tan y -1.271..-0.975 (P00 6.860, P11 6.751, the same
  1.016 aspect as every writer). The 256x256 Scorpion's is tan y
  -1.150..-1.090. Both aim 48 deg below V0's forward, at the radar's height
  0.215 m in front of V0, where the volume would be for a camera 0.65 m
  behind V0. That is the turret HUD's "about 0.7 m too far back" again.
- **The billboard** (vs 5559BD94) takes its corners in world space from its
  vertex buffer (b0[9..11] = [I | -camera]), projects them with the main
  eye's cb1[270..273], and passes the vertex UV through (`mov o1.xy,
  v1.xyxx`). EA02FAC2 samples t4..t6 at that UV and loads the scene depth
  t1 at SV_Position. The CPU builds the quad. In this flight every sampled
  5559BD94 billboard sat 2.1 to 2.9 m out (w), the radar's d1 and d2
  included, so the "holo quad" line (near billboards only) never fired.
- **Writer #6** (101961 vertices into 512x512, read by draw 0) uses the live
  camera with a window of tan x 0.156..0.258, tan y -1.032..-0.928 in
  round 1. It is not the radar's. The flight note's puzzle ("the disc is
  19 cm behind, yet #6 is in front") compared the radar with this other
  hologram.
  The 9216-vertex writer (#7, three targets, ps E95634B0's billboard) is
  live-camera too. Both billboards are family 7/8, so the radar group's
  translation moves them though their panels belong to the panels group.

Why no GPU fix: giving the picture's writer the moved camera, with a crop
and a billboard rectangle that hold the whole disc, needs, per eye and per
frame:
1. the live camera at the writer draws. They do not have it: the 4EDE draw
   binds no b1, and the 256x256 writer's b1 holds V0.
2. a way to tell the radar's 5B0068AF draw from the others on the CPU. Only
   its 256x256 target did so, in two rounds.
3. a new window from our own bounds.
4. a rebuilt billboard. The CPU makes its world-space quad, and its depth also
   drives the hologram shader's scene-depth discard.
When V0 updates (view entry, camera keys, recentre) is unknown. Every piece
would rest on two rounds at one setting. Fallback: camera back 70, radar
forward 0.

Seen in passing: the instrument's 12-texture cap dropped eye 1's draws 1
and 2 in round 1. A future round should also run while the radar group's
keys are 0, so it can see V0 at camera back 70.
