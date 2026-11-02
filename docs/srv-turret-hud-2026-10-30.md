# SRV turret view: camera position and HUD depth

## Status

- **State (2026-11-01, stripped):** the TEMPORARY issue-140 instruments are gone
  from the module, the rig and the log rule (branch `claude/trusting-jang-af02fa`,
  journal "the strip" below). Removed: the GPU instrument and its stream-output
  twins, the holo source, the radar camera V0, the unmoved-draw instrument, the
  pair capture and pictures, the seated lock, the draw-time correction, the
  residual and the setter's 10 s lines, the update counter, the third-writer
  note. Dead math went with them (`srv_turret_math.h`). Full local build green
  on the stripped tree. NOT FLOWN as stripped; merge to main waits on Sean's word.
- **Camera mode** (`experimental.srv_turret_radar_anchor = camera`, kept,
  FLOWN OK in e443b0c2): code path unchanged. The slot hook on 0x36BF3C0 moves the
  radar's schematic camera back; u is the turret's forward from the CPU camera
  sample (the Unmap tee), with the GPU readback kept as the fallback; the radar's
  own picture billboards take the GS +D move; every other 5559BD94 draw is the
  game's. The setter hook is no longer armed in camera mode (it published zero).
- **on and reversed** (kept as values, dead): the setter hook still adds the
  offset (directed along u, or the fallback). The draw-time correction is gone,
  so the radar billboards are left to the game and the disc orbits, as the
  flight of fe0b2560 showed. Not flown since the strip.
- **Keys** (`[experimental]`): twelve. Baked from the 2026-11-02 Frontier flight:
  camera back 80, HUD forward 0, radar forward 0, panels back 0, systems near 70,
  weapon panel up 10, gunner up 0. Screen limit is game, radar anchor is off,
  depth is game. `srv_turret_hud_up` raises the weapon panel and
  `srv_turret_gunner_up` raises the gunner frame, along the seat, at the game's
  own depth. The centre reticle and the Escape menu's 360-index draws stay.
- **Log lines that remain:** the settings, view, not-the-Scorpion, camera,
  anchor latch, first-draw, refused, geometry-stage and scissor lines; the 10 s
  `srv turret hud: panels:` line (its "unmoved family draws" part is gone); the
  camera-mode slot lines ("srv turret radar camera: slot hook armed", "moved
  back ... for slot", "10 s: slot calls"); the setter's "placement moved" and
  "view off" lines for on and reversed. The first-scope `srv turret hud state:`
  note is kept (not on the TEMPORARY list). The removed lines are no longer
  printed, and `tools\edvr_log.py` no longer reads them.
- **Proposed removals, awaiting Sean's word (nothing below is removed):**
  1. `experimental.srv_turret_radar_anchor` values `on` and `reversed`, with the
     setter hook (0x38563A0), its directed add, `anchorLeaves` and `g_anchorYield`.
     Dead modes; camera ships.
  2. The twelve `experimental.srv_turret_*` keys: ship the values as the behaviour
     and remove the keys when the arc closes (AGENTS.md "Scope control").
  3. The `srv_turret_hud_depth` values `game` and `real`, and the
     `srv_turret_hud_scissor` value `game`: candidates once the keys go.
  4. The geometry shader's TURRETPRE output (`srv_turret_hud_shader.h`): only the
     removed capture read it. Removing it changes the HLSL and the bytecode, so
     it needs its own build and flight.
  5. The GPU readback fallback for u (`copyVsRows`): only if the flights show the
     CPU path always wins.
  6. The settings line still says "TEMPORARY" in its text; the key is now
     `[experimental]`. The rule matches the line's tail, so the text changes with
     the keys.
- **Next flight:** none planned for the strip alone. To read one (Steam, the keys
  at their defaults, camera mode): the 10 s `srv turret hud: panels:` line
  (`edvr_log.py --grep "srv turret hud"`) and `srv turret radar camera: 10 s:`
  (slot calls and radar slots: radar slots above 0 with moves).
- **Open:** the flight of the stripped build; the two height keys; the proposals above.

## Evidence (flight of 2026-10-30, Epic, v0.22.0-rc.3-139-gcd4c91c4)

### 2026-11-02: the Escape-menu move was the wrong panel

Sean: the power and systems panel is not the Escape menu. Moving the 360-index
mesh of vs `5DAD41AA66757CE3`, vs `C4B4B334B26E81A9` and vs `4EF6DDB075A927FA`
(present only in dump `050120038002`) locked the headset view. That bind is
reverted. Dump `050105383001` (menu closed) and the gunner screenshot
identify the frame: the orange brackets, the bottom arc and the GUNNER
CAMERA label are the 2016-index sheet of vs `5DAD41AA66757CE3`; the target
card and the SYSTEMS card are the 288-index mesh of vs `C4B4B334B26E81A9`.
`experimental.srv_turret_systems_near` moves both toward the camera along
the turret, at the game's own depth. The centre reticle (vs
`E508648660A352B2`), the 6-index quad and the 360-index menu draws stay.
The hologram far window is no longer opened by this key. Placement defaults
are camera back 90, HUD forward 0, radar forward 0, panels back 110, and the
gunner distance ships at 65. `srv_turret_hud_up` raises the weapon panel and
`srv_turret_gunner_up` raises this frame, both along the seat, default 0.

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
- ruled out: V0 latching only at view entry, because in flight a5800fdc
  (`edvr_gfx_20261031_135051.log`) the view stayed on from 13:52:52 to
  13:54:53 with no re-entry, and V0 snapped to the live camera at camera back
  55 (13:53:21.356) and again at 50 (13:54:38.787).
- ruled out: V0 always following the head, because it stayed frozen for
  2619 frames from view-on through camera back 5 to 50, and again from 55
  down to 0.
- ruled out: camera back never reaching V0, because from the snap on V0
  tracked the live camera, camera back included, every frame (the origin at
  z +0.09 to +0.28).
- ruled out: an offset in obj+0x15B0/0x15F0 alone showing the radar,
  because any nonzero value drops the schematic about 1.96 m straight down
  (flight 79bb61ba, `edvr_gfx_20261031_150213.log`). The drop is the same
  for +0.40, -0.40 and -0.50, and switches on and off one frame after the
  offset does, so the radar is never in view and V0 never unfreezes.
- ruled out: (0, 0, F) in the anchor's axes as the radar's forward move,
  because it runs along the vehicle's forward, not the turret's: the disc
  swings with turret yaw and sits about 5 cm above the shader-moved icons
  (flight 07bde936, `edvr_gfx_20261031_160808.log`).
- ruled out: P's rows as the source of the turret's direction, because they
  stay near identity (8 deg of tilt at most) while the turret turns -115 to
  +150 deg (flight c1275025, `edvr_gfx_20261031_191536.log`).
- ruled out: a billboard move showing the disc at camera back 25, because
  the picture is empty there (the volume is behind V0); dump 130153201001
  has no disc at all.
- ruled out: placement as the cause of the radar's cut and stretch, because
  camera back 25 + radar forward 45 (sum 70, flight 82546e51) still cuts it:
  a moved billboard shows a picture taken from the unmoved place.
- ruled out: one forward pull making the layers meet, because a pull only
  moves vertices deeper than gap + 0.1 m. The near layer (0.11-0.70 m) is
  never in the window, and the deep layer itself spans 1.3-2.8 m.
- ruled out: predicting the turret's aim (any lead) to hold the disc steady,
  because with the timing right (residual mean about 0, implied latency
  0.00 frames, lead at the 11.1 ms horizon) the residual still spreads
  +-1 to 2 deg while sweeping, lagging and leading about evenly (flight
  c8d79c6a, `edvr_gfx_20261101_040003.log`).
- ruled out: the billboard's t1 scene-depth discard as the Scorpion ghost,
  because with a far t1 on every moved draw (t1 far 2932, kept 0) the ghost
  stayed (flight e64866f9, `edvr_gfx_20261101_055340.log`).
- ruled out: a far disc quad (the disc moving less on screen than the icon
  because its quad sits deeper), because the quad probe read disc w 0.297 to
  0.305 m, icon w 0.315 to 0.323 m and z_c 0.31 to 0.33 m: both quads
  already sit at the content's depth, and "moved by" equalled "expected" for
  both (flight 15b659b2, `edvr_gfx_20261101_062714.log`).
- ruled out: correcting the radar by the setter's last d (the delta mode),
  because the game damps the placement over about 65 ms, differently for
  the disc and the icon: at -130 deg/s the disc's pre-move centre sat about
  9 cm and the icon's about 7 cm off rest, and after the delta still 5.3 and
  3.1 cm (flight 8e3bcd7b, dump 072742058001).
- ruled out: a hole in the disc's picture as the Scorpion's ghost, because
  the dump's disc t4 (`eye_072742058001_RadarDiscT4.bmp`) is a full ellipse;
  the ghost is the disc's quad 2 cm off the icon's (flight 8e3bcd7b).
- ruled out: a seated lock of the radar billboards' quad centres, because
  the quads held their targets (centre off the target 0.00 mm, shifts 2 to
  8 cm, max 25 cm) while the pictures in them are drawn from a viewpoint 10
  to 25 cm wrong: the native content itself swings +-9 cm sideways in the
  camera frame during sweeps, and Sean saw the disc "sort of orbit up and
  down" (flight fe0b2560, `edvr_gfx_20261101_080045.log`).

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

### 2026-10-31: the radar camera on the CPU side (RE in progress; instrument built, not flown)

Sean wants the radar drawn right near the stock camera (back 0 to 35), so
the fix has to come from where the game makes V0. Read in build 334925
(the Steam install's, from the log's "game build 334925"), with Ghidra 12.1.3
headless on ED334925, read-only. The notes are in the analysis inbox item
`20261031-133850-srv-radar-camera`, to be filed as
`analysis\decomp\verify_20261031_srv_radar_camera.txt`.

- The pictures come from the game's Schematics system (the
  `SchematicsComponent.cpp` path string). The radar disc is a
  SchematicTerrainMaterial; its parameter block is built at 0x383D700:
  ClipRadius, CameraOffset, GroundOffset, ShipForward, PhasedOutY,
  PhaseInLerpValue, OSToWS_Rotation, OSToWS_Translation. From the 4EDE vertex
  shader, "CameraOffset" is the clip cylinder's centre (cb2[0].xyz), not the
  picture's camera.
- 0x36BC240 is the schematic manager's constructor (vtable 0x54FB8D0). It
  makes the views "SchematicPrimary" and "SchematicSecondary" and N slots:
  0x36BC660, slot vtable 0x54FB7F8, each with a 0x900-byte companion
  object. 0x36BD2C0 and 0x36BD7C0 name the slots' targets
  "Schematic_%s_%u" and "Schematic_Depth_%u". 0x36B4840 makes the
  "SchematicBillboardView" buffers. Slot method [5] (0x36BE670) submits a
  slot's render.
- Not found: where a slot's camera is written. Next to read: the slot
  constructor 0x36BC660 and its 0x900-byte companion, the manager's
  per-frame update (called directly from 0x282BB00's class), and the
  callers of the target creators.

The open RE questions are what V0 is copied from, when it updates, where
its framing comes from, and whether camera back reaches it. Without them a
CPU-side hook would be a guess. So this build carries an instrument
instead, "the radar picture's camera" (srv_turret.cpp; the decode is
srv_turret_math.h radarCamFrom, which the rig checks against this flight's
logged constants):
- While the Scorpion's turret view is on, the frame's first vs 4EDE4BC9 draw
  has its cb0 rows 4..11 copied and read back without stalling.
- Each move of V0 (2 mm or 0.05 deg) is logged with the frame, the head's
  seated position and the camera back.
- The view's end gives a count, with a NEVER SEEN line when nothing was
  read.

What it decides, per line:
- **Camera back reaches V0:** a "moved" line at each camera-back step, with
  the schematic's z changing by the step.
- **V0 latches at view entry:** no moves within a stay, and a new "first
  seen" after each re-entry.
- **V0 follows the head after all:** moves that track the head.
- **A distance or timer latch:** moves only after a lean past some
  threshold, or at a regular interval.

### 2026-10-31: V0 is gated by visibility (flight a5800fdc); the anchor found

Flight a5800fdc, Steam, `edvr_gfx_20261031_135051.log`. The view stayed on
from 13:52:52 to 13:54:53. The view-off line: 10610 draws copied, 10610
read, 3054 moves.
- **Up from 0.** View-on at camera back 0 was "first seen" with the origin
  at z -0.437. V0 then stayed frozen for 2619 frames through camera back 5
  to 50.
- **At 55** (13:53:21.356) V0 snapped to the live camera, moved by
  (-1, -75, 526) mm; the origin was now at z +0.089. From then on it tracked
  the live camera every frame, through 70 (the origin at z 0.24 to 0.28),
  head turns and leans included.
- **Back down.** It still tracked at 60 (z 0.151), and froze from 55 down
  to 0.
- **Up again.** It stayed frozen up to 50 (13:54:38.787), when it moved
  after 2310 unchanged frames (z 0.058), and tracked again.

So V0 is the live camera, camera back included, but it updates only while
the radar volume is in view; otherwise the writer redraws with stale
constants. The snap points vary with head pitch (z 0.058 to 0.10 right
after), which is a visibility test, not a distance.

The RE (subagent, build 334925, read-only;
`analysis\decomp\verify_20261031_srv_radar_anchor.txt`, filed as
analysis commit 9f71604) agrees and finds the anchor:
- **The gate.** 0x287B050 calls 0x36BF330 with the main camera, which
  calls 0x36BF3C0 for each slot. 0x36BCEB0 projects each object's local box
  with world x the camera's VP, and 0x36BE0F0 clips its 12 edges against the
  near plane. A slot takes the camera, its window and the billboard
  rectangle only when some edge survives and the merged rectangle has area.
- **The placement.** The radar's terrain-schematic object (constructor
  0x383DF40, vtables 0x5511DE8 at +0 and 0x5511F18 at +0x58) is updated
  per frame by 0x384A590(obj in RCX, dt in XMM1), reached only through the
  thunk 0x381EF00. Its world-matrix writer 0x385DC20 builds world = S . X .
  M . A:
  - A is the anchor entity's transform (obj+0x140).
  - M is a local offset whose translation is obj+0x15B0 / obj+0x15F0 (lerped,
    in metres, in the anchor's axes). The constructor zeroes it, and nothing
    else writes it.
  The Scorpion icon's schematic reads the radar's M through obj+0x58, so it
  should follow too (unconfirmed).
- **Open** (the intake's list): which entity the anchor is and whether it
  turns with the turret; whether ships create this class; whether the slot's
  region box (slot+0x310, test 0x5E1830) can reject a moved hologram.

### 2026-10-31: the radar anchor moved in the game (built, not flown)

The anchor above is where the game places the radar's schematic, so this
build moves the schematic there instead of moving its picture afterwards
(`src\d3d11\srv_radar_anchor.cpp`, with the TEMPORARY key
`fix.srv_turret_radar_anchor`: `on`, `reversed` or `off`).

- **The hook.** An entry hook on 0x384A590, the schematic's per-frame update
  (obj in RCX, dt in XMM1, reached only through the thunk 0x381EF00), for
  build 334925 only. Before it patches anything it checks the PE identity
  (stamp 0x6ADA8048, image size 0x640C000) and the function's first 16
  bytes, `40 53 56 48 81 EC 68 01 00 00 0F 29 BC 24 30 01`, read from the
  exe through its section table. CodeHook moves the first three
  instructions (10 bytes); the RIP-relative one at +0x12 is not reached. On
  any other build, or other bytes, it stands down and patches nothing. It
  is armed the first time the Scorpion's turret view comes on with the key
  not off.
- **What it writes.** Before the original runs, on an object whose vtables
  are base+0x5511DE8 at +0 and base+0x5511F18 at +0x58, the offset (0, 0,
  s x radar forward) in metres, w 0, goes into both local translation rows,
  obj+0x15B0 and obj+0x15F0 (0x385DC20 lerps between them). s is +1 for
  `on` and -1 for `reversed`. The frame tick publishes the offset only
  while the Scorpion's turret view is on, VR only. Otherwise the offset is
  zero, and a schematic the hook wrote gets zeros on its next update. `off`
  publishes zero, which is the old geometry-shader behaviour. The callback
  stores integers only before it calls the original, so XMM1 (dt) reaches
  the game intact; the rig pins that text.
- **The billboards.** While an application is at most 10 frames old and
  the key is not off, the geometry shader leaves the two 5559BD94 families
  (EA02FAC2, the disc's picture; E95634B0, the Scorpion) alone: the game
  places them from the moved schematic. Their draws count as group-off in
  the 10 s line's unmoved list. The other radar families (DF3503CD,
  44DDCDEB, 9B34C331, 9611A454, B932058F) keep the shader's radar-forward
  move along the seat.

Unconfirmed until it flies:
- the anchor's axes and sign: +Z is forward only by the material's name,
  "ShipForward", for the anchor's row 2;
- whether the anchor turns with the turret (if it does not, the offset is
  along the SRV's forward, not the view's, and the disc can part from the
  shader-moved radar families);
- whether ships create this class too (the vtable check and the Scorpion
  turret gate keep the write off anything else);
- whether the slot's region box (slot+0x310, test 0x5E1830) can reject a
  moved hologram.

The log lines (graphics log; the frame tick says them, the hook never
logs):
- `srv turret radar anchor: hook armed: EliteDangerous64.exe+0x384A590 (the
  radar terrain schematic's update, build 334925) at 0x..., stolen=10
  bytes, prologue 16/16 bytes verified, relay at 0x...`, or `srv turret
  radar anchor: hook stood down: <why>; nothing was patched`.
- `srv turret radar anchor: applied at frame F: offset (x,y,z) m in the
  anchor's axes (key on|reversed), obj 0x...; the 5559BD94 billboards are
  left to the game`, once a stay.
- `srv turret radar anchor: 10 s: applied in N frames (C calls through the
  gate, M with the radar schematic, R restores, F faults, S without a
  slot), offset (x,y,z) m (key K), last obj 0x...`, beside the 10 s line.
- `srv turret radar anchor: view off: applied in N frames`, ending ` --
  NEVER RAN: the radar schematic's update was not called with a matching
  object while the view was on` when N is 0 and the hook is armed, ` --
  hook not armed: <why>`, or ` -- not asked: ...` (key off, or radar
  forward 0).
- The settings line gains `, radar anchor on|reversed|off`.

To fly: camera back 0 to 35, radar forward = 65 minus camera back (45 at
20, 30 at 35), `fix.srv_turret_radar_anchor = on`. The "srv turret radar
camera" instrument is unchanged. Once the anchor applies, V0 should start
tracking the live camera, and its "moved" lines should show the
schematic's origin z risen by radar forward (about +0.2 m at back 0 with
65). If the origin z goes MORE negative after the anchor applies, or V0
stays frozen and the disc stays empty after the "applied" line, switch the
key to `reversed` in flight.

### 2026-10-31: the anchor offset drops the radar 1.96 m (flight 79bb61ba)

Steam, `edvr_gfx_20261031_150213.log`, build matches. The view was on from
15:04:17 to 15:06:30. Dump 150611556001 has no disc, only the floating
sprites.
- **The hook works.** "hook armed", then "applied" every frame (about 887
  in each 10 s, 0 faults, 0 without a slot); at view off, "applied in 10361
  frames".
- **The axis and sign are right.** Each 5 cm step of radar forward moved
  the schematic's origin (2, 10, 49) mm in V0's frame, about 1:1 along V0's
  forward (V0 is pitched about 7 deg up). Going from `on` +0.40 to
  `reversed` -0.40 moved it (-40, -167, -781) mm, so `on` is forward.
- **The drop.** With any nonzero offset the origin sits at y -2.1 to -2.3
  in V0's frame, against -0.25 with zero.
  - At view-on with +0.40 it was first seen at (-0.004, -2.095, 0.332).
  - At 15:06:01 the key went off. That frame only the translation came
    back, (20, 84, 391), with y still at -2.18. The next frame it moved
    (47, 1933, -328) to y -0.246, and the window and gate looked normal.
  - At 15:06:03, back on: (-26, -1849, 719), the +0.40 translation plus
    the drop.
- The drop is about (-47, -1933, +328) mm in V0's frame: 1.96 m, nearly
  straight down along gravity once the pitch is taken out. It is binary and
  follows the offset by one frame. V0 stayed frozen at its view-on pose all
  flight, since the dropped radar was never in view.
- Open: what in the game reacts to a nonzero M. Candidates: a phase-out
  (PhasedOutY / PhaseInLerpValue or a phase state on the object); the slot
  region box at slot+0x310; or 0x15B0/0x15F0 being a phase lerp pair or a
  ground-relative mode rather than a plain offset. The drop is about the
  camera's height above the vehicle's ground point, and the world matrix's X
  frame is recentred on that point.

### 2026-10-31: the radar placement moved through its setter (built, not flown)

**The cause** (RE, build 334925, read-only;
`analysis\decomp\verify_20261031_srv_radar_drop.txt`, waiting in the
analysis inbox as `20261031-151930-srv-radar-drop`). The earlier note was
wrong that nothing writes obj+0x15B0: the game writes the placement
through the schematic's interface at obj+0x58, whose slot +0x28 (vtable
0x5511F18, entry 0x5511F40) is the setter 0x38563A0. It takes RCX =
obj+0x58 and RDX = a 4x4 P (rows x, y, z, translation) and stores P at
obj+0x1580..0x15BF (so P's translation lands at obj+0x15B0), P raised by
0.1 x its own up row at obj+0x15C0..0x15FF, and the size at obj+0x1600.
P's translation is the hologram's place relative to the anchor, in the
anchor's axes, and on the Scorpion it is about (0, +1.96, 0). The anchor
sits near the vehicle's ground point, so the hologram is 1.96 m above it.
The first build REPLACED that translation with (0, 0, F), so the radar fell
1.96 m (see "Ruled out").

**The fix** (`src\d3d11\srv_radar_anchor.cpp`):
- **The setter hook.** An inline detour at 0x38563A0, build 334925 only.
  The PE identity and the first 18 bytes are checked first: `48 83 EC 48
  0F 10 02 0F 11 81 28 15 00 00 0F 10 4A 10`, read from the exe through its
  section table. CodeHook moves the first two instructions (7 bytes:
  `sub rsp,48h; movups xmm0,[rdx]`). Its decoder now takes movups (0F 10 /
  0F 11, no F2/F3 prefix), with unit cells in `vtable_test`; every other SSE
  form is still refused. The object must have vtables base+0x5511F18 at
  RCX and base+0x5511DE8 at RCX-0x58.
- **The rule** (`srv_turret_math.h` `radarPlacementMove`, which the rig
  checks). P is copied, d = (0, 0, s x radar forward) is ADDED to its
  translation, and the original runs on the copy. It never adds twice. Each
  schematic (4 slots) remembers the game's translation it moved, the
  output and d. A translation bit for bit equal to that output is our own
  value fed back: it passes through unchanged while d is unchanged, and is
  re-based on the game's value when d changes. With d zero that puts the
  game's value back. Otherwise the game's own next setter call restores
  its place once d is zero. EDVR never calls the setter: a key change
  reaches the game at its next call.
- **The update hook** (0x384A590) now only counts updates with the radar
  object. It writes nothing and stays integer-only.
- **The billboards.** The two 5559BD94 families are left to the game while
  the last radar setter call left our offset in the placement (added, or
  ours fed back). That is "an add within the last frame" if the setter runs
  every frame, and stays right if it runs rarely, where a 10-frame window
  would move the billboards a second time.

Unconfirmed until it flies:
- the setter's caller (its return address is logged) and its call rate.
  The RE suspects once a frame, from the one-frame lag in flight 79bb61ba;
- accumulation: a caller that feeds our value back bit for bit is handled,
  and one that feeds it back in another form raises the WARNING below;
- whether DF3503CD and the other radar families (the contacts) share this
  placement. They keep the shader's radar-forward move, so if they now sit
  off the disc, they do not.

The log lines (the frame tick says them; the hooks never log):
- `srv turret radar anchor: setter hook armed: EliteDangerous64.exe+0x38563A0
  (the radar schematic's placement setter, build 334925) at 0x...,
  stolen=7 bytes, prologue 18/18 bytes verified, relay at 0x...`, or `...
  setter hook stood down: <why>; nothing was patched`. Then `srv turret
  radar anchor: update counter armed: EliteDangerous64.exe+0x384A590 (the
  radar schematic's update, counted only, nothing written) ...` or `...
  update counter stood down: ...`.
- `srv turret radar anchor: placement moved at frame F: the game's (x,y,z) +
  (0,0,d) -> (x,y,z) in the anchor's axes (key on|reversed), caller
  EliteDangerous64.exe+0x..., obj 0x...; the 5559BD94 billboards are left
  to the game`, once a stay.
- `srv turret radar anchor: 10 s: setter calls N (radar M, moved A, already
  ours R), the game's translation (x,y,z)[ changed K times], updates U`,
  then the restores, faults and offset, `; the placement holds our offset`,
  and ` -- NO setter call with the radar in these 10 s: ...` when M is 0.
- `srv turret radar anchor: view off: moved in N calls`, with ` (already
  ours in R)`, and ` -- NEVER RAN: the radar schematic's placement setter
  was not called with the radar object while the view was on`, ` -- hook
  not armed: <why>` or ` -- not asked: ...`.
- `srv turret radar anchor: WARNING: the game's translation moved by (..)
  between setter calls, close to the offset: its caller may feed back our
  value`, once.

To fly: camera back 0 to 35, radar forward = 65 minus camera back (45 at
20, 30 at 35), `fix.srv_turret_radar_anchor = on`. In "srv turret radar
camera" the origin's z should rise by radar forward with no drop in y, and
V0 should start tracking once the radar is in view.

### 2026-10-31: the setter flown (flight 07bde936); the radar placement along the turret (built, not flown)

Steam, `edvr_gfx_20261031_160808.log`, build matches, dump
`eye_161017547001`. The radar disc shows (rim, fill and the Scorpion icon),
and V0 tracks the live camera the whole stay, with the origin's z 0.10 to
0.30 m ahead.
- **The setter.** It armed and ran every frame: about 900 calls in each
  10 s, all with the radar, "already ours" 0, no WARNING. The game's
  translation is (-0.008, 1.959, 0.077..0.094) in the anchor's axes, and it
  barely changes as the turret turns.
- **No view-off leftover.** Because the setter runs every frame, the game's
  own value is back the frame after the view goes off.
- **DF3503CD and the contacts are not on this placement.** DF3503CD's
  disc-plane line stayed at seated (-0.008, -0.215, 0.424) all flight. They
  move only by the geometry shader, radar forward along seated -z.
- **Two problems, one cause** (Sean):
  - The disc sits about 5 cm too high. The yellow heading tick is 5 cm below
    the disc's lower rim; at camera back 70 it sat on the rim.
  - The disc is locked to the vehicle, not the turret: turning the turret
    swings it off to the side.
- **Reading.** The game's placement puts the radar on the turret's yaw axis
  (x and z about 0 in the anchor's axes). The offset (0, 0, F) runs along
  the VEHICLE's forward, so the disc leaves that axis and swings as the
  turret yaws. The seated forward is about 7.5 deg below the vehicle's
  level (DF3503CD's seated normal is (0, 0.992, 0.130)), so the shader
  drops the icons about 0.40 x 0.130 = 5 cm, while the disc moves level.

**The build.** The offset is now s x F along the turret's seated forward u,
the same direction the shader moves the icons, put in the anchor's axes:
d = s F (u . A_row0, u . A_row1, u . A_row2). The key is unchanged: `on`
is +u, `reversed` is -u, `off` is none. The add-not-replace and
idempotence rules are as before; only d changes.
- **u (render thread).** For eye 0's first panel or 5559BD94 draw of each
  frame, the live camera's world axes are copied from VS b1 rows 277..279
  (right, up, forward). They are paired with eye 0's seated rotation R of
  that same frame, the host's pose at the draw, as `drawEyeFrame` uses. A
  3-slot staging ring is read back at the frame boundary without stalling.
  Then u = e.x right + e.y up - e.z forward, with e = R^T (0, 0, -1) in
  OpenVR's axes (`srv_turret_math.h` `turretForwardWorld`). The camera's
  rows must be unit and orthogonal within 0.05. The frame tick clears u
  when a stay begins.
- **A (the setter hook).** The anchor is *(obj+0x140). Its world 4x4 comes
  from its vfunc +0x20, which returns a pointer to rows x, y, z and
  translation. That is the call 0x3843530 makes for the region box: `mov
  rcx,[iface+0E8h]; mov rax,[rcx]; call [rax+20h]`, read from the exe. The
  hook calls it only when its vtable and the getter lie inside the game's
  image, under SEH, and only while there is an offset to add. The rotation
  rows must be unit and orthogonal within 0.05 (`radarAnchorAlong`).
  Otherwise, or with no u yet, d falls back to (0, 0, s F), and the
  fallback is counted.

Unconfirmed until it flies:
- that the anchor's matrix and the camera's rows share one world frame. A
  turret straight ahead should give an angle to the anchor's z of about
  7.5 deg and a yaw of about 0;
- that the anchor's rows stay put through a turret sweep while d's yaw
  follows the turret, and what P's rows do;
- whether the u read back two to three frames late shows during a fast
  sweep.

The log lines (the frame tick says them):
- `srv turret radar anchor: direction at frame F: u (x,y,z) world; anchor
  rows (..)(..)(..) [source vfunc +0x20]; P rows (..)(..)(..); d (x,y,z) in
  the anchor's axes = D m along u (angle to the anchor's z: A deg, yaw Y
  deg; key K)`, with the first move of a stay and then every 2 s while the
  view is on. A fallback reads `[fallback: no u yet]` or `[fallback: the
  anchor's rows not read or refused]` and `along the anchor's z`.
- The 10 s line gains `moves along u D, fallback B (no u N, anchor not read
  or refused M; getter faults G); camera reads R of C copies (J refused);
  last d (x,y,z) m in the anchor's axes (key K)`.
- "placement moved at frame F" now prints the d it added.

To fly: camera back 0 to 35, radar forward = 65 minus camera back,
`fix.srv_turret_radar_anchor = on`. Sweep the turret about 90 deg left and
right, and watch the disc's rim against the yellow heading tick.

### 2026-10-31: the turret direction flown (flight c1275025); the turret direction without the readback (built, not flown)

Steam, `edvr_gfx_20261031_191536.log`, build matches, dump
`eye_192101956001`. Sean: "almost there". The disc follows the turret, and
the height is right: the disc, the Scorpion icon and the contact on the rim
line up. But the disc lags the turret slightly while it turns. Driving was
not tested.
- u tracked the turret through yaw -115 to +150 deg against the anchor's z.
  The anchor's rows stayed constant (the vehicle was parked).
- No failures: 900 of 900 moves along u in steady windows. The only fallback
  was the first frame (no u yet), with 0 refused, 0 getter faults, and as
  many camera reads as copies.
- P's rows stay near identity (8 deg of tilt at most) and do not carry the
  turret's yaw (see "Ruled out").
- d's vertical part varies with yaw: d.y / 0.4 runs from about +6 to -12 deg.
  It is probably gun elevation, since the icons ride the same u. A pairing
  error between the camera sample and the head pose is not yet excluded.
- The lag is most likely u's age: the GPU readback made u two to three
  frames old, about 1 to 2 cm at 0.4 m during a brisk turn.

**The build.** u reaches the setter with the least latency available, and
whatever age is left is measured and carried forward.
- **The camera rows on the CPU.** The 5376-byte scene camera block (the one
  the camera-glitch, camera-sunglare and camera-temporal readers see at
  Unmap) holds the camera position at float 1100 and right, up and forward
  at rows 277..279. A new reader in the Unmap tee
  (`srvTurretNoteCameraWrite`) keeps those rows by buffer with the host's
  frame sequence. At the eye-0 draw the bound VS b1 is looked up there
  first. The rows its last write put there are this draw's, because they
  were written before it on the same thread. The sample is used when they
  were written in the frame of the pose at the draw, and bound from the
  first constant. u is computed at once, with no readback. The GPU ring
  stays as the counted fallback.
- **Pairing by frame.** The setter keeps the anchor's rotation by host frame
  (8 frames). It puts u into the anchor's axes with the rotation of the
  sample's own frame, so the vehicle turning or driving adds no error. That
  aim depends only on the turret relative to the vehicle.
- **Extrapolation.** The aim (yaw, pitch relative to the vehicle) is carried
  over u's age in frames (setter frame minus sample frame), at the rate
  between the last two samples. The rate is held to 4 deg a frame, the age
  to 4 frames and each step to 5 deg, so a reversal overshoots by 5 deg at
  most (`srv_turret_math.h` `extrapolateAim`).
- The key, the fallback, the add-not-replace rule, the V0 instrument and the
  shader's move of DF3503CD and the contacts are unchanged.

Unconfirmed until it flies:
- whether the eye-0 draw's VS b1 is the 5376-byte block the tee sees, and
  written in the pose's frame. The first sample says "from the CPU" or why
  it went to the GPU, and the 10 s line counts each reason;
- u's age with the CPU path: 0 if the setter runs after the render in the
  same frame, 1 if before;
- whether d's pitch is gun elevation. A pairing error would make the aim's
  pitch follow the head's pitch; gun elevation would not.

The log lines (the frame tick and the render thread's first-time notes):
- `srv turret radar anchor: turret direction from the CPU: eye 0's VS b1 is
  the scene camera block (N bytes), rows 277..279 read when the game wrote
  them in host frame K; no GPU readback`, or `... turret direction from the
  GPU readback for host frame K (<why>); counted in the 10 s line`, once.
- The direction line gains `; u age N frames (setter frame F, camera sample
  frame G), extrapolated by X deg yaw Y deg pitch[ (held)] (yaw rate R
  deg/s), source cpu|gpu, anchor of the sample's frame|of the setter's
  frame (the sample's not kept); aim relative to the vehicle yaw A pitch B
  deg, head pitch H deg`.
- The 10 s line gains `; camera samples cpu N, gpu reads R of C copies (J
  refused; cpu missed: not written a, offset b, another frame c, not a
  camera d); u age mean M max X frames; extrapolated mean E max F deg, held
  H; anchor paired by frame P of Q`.

To fly: camera back 0 to 35, radar forward = 65 minus camera back,
`fix.srv_turret_radar_anchor = on`. A turret sweep, then drive forward and
turn the vehicle, watching the disc against the turret.

### 2026-10-31: the CPU direction flown (flight a8465cb3); a lead in time (built, not flown)

Steam, `edvr_gfx_20261031_213131.log`, build matches, dump
`eye_213337152001` (taken mid-sweep; it looks right). Sean: the disc keeps up
with the turret now, but it vibrates while the turret moves.
- **The CPU path works.** "turret direction from the CPU" (the 5376-byte
  scene camera block): a CPU sample about every frame, 0 GPU reads, 0 CPU
  misses, and the anchor paired by frame about 87 to 90 % of the time.
- **u's age** was mean 0.04 to 0.39 and max 1. The direction lines read age
  0 nearly always (setter frame equal to sample frame), with an occasional
  age 1 that was extrapolated by 1.6 to 4 deg.
- **The turret's yaw rate** while Sean swept was 90 to 170 deg/s: 1 to 2 deg
  a frame, about 1 cm at 0.4 m.
- **The pitch is the gun's.** The aim's pitch held at -0.4 to -18 deg while
  the head's pitch changed differently, so the pairing is sound.
- **Reading** (unconfirmed). A setter call always sits between two camera
  writes, and the next write shows its placement. So the right lead from the
  latest sample is about one sample interval, whichever host frame the call
  falls in. The host frame's boundary falls sometimes before the call (age
  1) and sometimes after it (age 0), so leading by the age flipped between 0
  and 1 frame. At 1 to 2 deg a frame that is a jump of about 1 cm between
  frames: the vibration. A constant error would only lag; a flipping one
  shakes.

**The build.**
- **A lead in time.** The aim is carried from the latest sample by a time,
  the lead, instead of by its age in host frames. The lead is the display
  horizon: from the base sample's camera write to the write that shows the
  call. It is measured at every new sample (the write shows the most recent
  setter call before it) and adapted a 32nd of the way each time (about
  0.35 s at 90 Hz). It starts at one 90 Hz frame (11.1 ms) and is held to 0
  to 40 ms.
- **A smoothed rate.** The rate is taken end to end over the last 4 samples
  within 100 ms, by their write times (QPC), and held to 360 deg/s. Each
  lead step is held to 5 deg. In practice a reversal overshoots by about
  the rate times the lead, roughly 2 deg at 170 deg/s, for about three
  samples.
- **The residual instrument.** At each camera write the true aim (that
  camera with its own pose, in the anchor's axes of its frame) is compared
  with the aim used by the setter call the write shows. The 10 s residual
  line gives the mean, the max |.|, lagging against leading, the implied
  latency left in frames, the measured horizon, the lead, and the setter's
  host-frame ages.
- **No render-time correction.** The lead depends only on times the hook
  measures, so the setter side should come out steady. If the residual
  line still shows a spread like the rate times one frame, the next step is
  the geometry-shader residual for the 5559BD94 billboards.
- The key, the fallback, the add-not-replace rule, the V0 instrument and
  the shader's move of DF3503CD and the contacts are unchanged.

Unconfirmed until it flies:
- that the write after a setter call is the one that shows it. If the
  residual shows a steady lag of about one frame with the lead at the
  horizon, the placement is shown one write later and the lead needs one
  more interval;
- that the residual's mean is near 0, its max small, and lagging and
  leading about even, in fast and slow sweeps and while driving.

The log lines:
- the direction line's timing now reads `u age N frames (setter frame F,
  camera sample frame G), led by X deg yaw Y deg pitch[ (held)] over L ms
  (sample interval I ms; yaw rate R deg/s over the last 4 samples), source
  cpu|gpu, ...`;
- the 10 s line's `extrapolated` now reads `led by mean E max F deg, held H`;
- a new line every 10 s: `srv turret radar anchor: 10 s residual: yaw true
  minus used mean M max |X| deg over N writes (lagging A, leading B, the
  rest too slow to say), implied latency mean L frames over n, display
  horizon mean H ms, lead now D ms, sample interval I ms; setter ages 0: a,
  1: b, 2+: c`. When the lead is right, M is near 0 and X is small.

To fly: camera back 0 to 35, radar forward = 65 minus camera back,
`fix.srv_turret_radar_anchor = on`. Fast and slow turret sweeps, then drive
forward and turn the vehicle.

### 2026-11-01: the time lead flown (flight c8d79c6a); a correction at draw time

Steam, `edvr_gfx_20261101_040003.log`, build matches, dump
`eye_040252262001` (it looks right in a still). Sean: the radar still
jitters noticeably while the turret turns; he wants it smooth and in place.
- **The timing is right.** The 10 s residual lines read a mean of -0.003 to
  +0.003 deg (-0.06 and -0.13 in the last two windows), an implied latency
  of 0.00 frames, and a display horizon and lead of 11.1 ms. Lagging and
  leading split about evenly (160 and 177, 125 and 118, 60 and 72).
- **The spread is not.** The max |residual| while sweeping was 0.94, 1.21
  and 1.98 deg. The 12.5 and 62.3 deg maxima of the last two windows came
  from frame-time spikes near view-off.
- **Reading.** The lead removes the mean lag. What is left is +-1 to 2 deg
  of symmetric noise: Sean's changes of input, which no prediction can
  foresee. At 0.4 m that is about 1 cm from frame to frame, the jitter.
  Prediction cannot hold the disc steady; its place has to be corrected
  when the frame is drawn, from that frame's own camera.

**The build.**
- **No lead.** The setter uses the latest camera sample as it is: d = s F
  along u in the anchor's axes of the sample's frame. The time lead, the
  smoothed rate, their caps and their rig cases and pins are removed. If
  the pairing of a write with its setter call were ever off by one call, an
  unled aim makes the error a smooth one-frame lag, where a lead made it
  jitter.
- **The call ring.** Each radar setter call goes into an 8-entry ring under
  a version per entry (odd while the setter writes it; no lock, allocation
  or SEH): its QPC time, the base sample's write time, the offset it left
  in force (the d added; for our value fed back, the remembered d; else 0,
  which counts as no shown call), the anchor's world rows it read, and s F.
  A camera write shows the most recent call before it.
- **The correction.** At each draw of the radar's own picture billboard,
  the draw is moved through signature H's geometry shader by delta =
  desired - used, in the world (`srv_turret_math.h` `radarCorrectionWorld`).
  - used is the shown call's offset put through its anchor rows; desired is
    s F along this frame's true u (that draw's camera rows, read when the
    game wrote them, with that eye's pose of the same frame) in the same
    anchor's axes. A uniform anchor scale then applies to both.
  - delta is worked out once a frame, at the frame's first radar billboard
    draw, and each eye turns it into its own space with its own camera rows:
    (R . delta, U . delta, F . delta), rows 277..279 of that draw's VS b1.
    That is the inverse of signature H's lighting line, so the lighting
    position moves by delta too, with no shader change; it holds on canted
    displays.
  - The geometry shader gets the translation alone (gap 0) and the game's
    own depth (mode game, whatever `fix.srv_turret_hud_depth` says), so the
    hologram's scene-depth discard is unchanged. A delta over 10 cm (a
    view-off frame, a fallback add) leaves the draw to the game.
- **Which billboards.** While the yield is on, the draws of the terrain
  schematic (vs 4EDE4BC9) and of vs 5B0068AF into a 256x256 target (the
  Scorpion icon, from the texture's own size) record their render targets
  for the frame. A family 7 draw (ps EA02FAC2) is moved only when its PS t4
  is one of this frame's. A target only last frame's radar writers drew
  into is counted but never moved: the game's hologram targets can change
  hands between frames (the holo rounds of flight 707c0ab5), so it may now
  hold a panel's picture. Family 8 (E95634B0) and the 512x512 panel
  hologram are never moved; a family 8 draw on a radar target is counted,
  because that would contradict the model.
- **The desired offset is the setter's own.** dStar is what the setter would
  set for this frame's u with the shown call's anchor rows (the same
  `radarAnchorAlong`), so the aim the call used gives a delta of exactly 0
  even if those rows carry a small scale.
- **Instruments.**
  - A 10 s correction line whenever the yield was on in the window, even
    when nothing moved.
  - A first-application line, once.
  - The setter's and the camera writes' threads, once.
  - In the residual line, how long before the camera write the shown call
    was (near 0 would mean a racy pairing).
  - The residual is classed against the plain change of aim between the
    last two samples; with no lead its mean is the turret's motion over the
    horizon, which the correction takes up.

Unconfirmed until it flies:
- that the shown call is the one displayed. A smooth one-frame lag after
  the correction would mean the previous call is shown;
- that the 256x256 rule picks only the radar's icon. The correction line
  counts frames with two or more such writers;
- that the billboards are drawn after their pictures in the same frame. If
  "no writer match" is high with most of it on last frame's radar target,
  the pictures are drawn after their billboards.

The log lines:
- `srv turret radar anchor: 10 s correction: N billboard draws moved (eye 0
  a, eye 1 b; disc c, icon d) over F frames, |delta| mean X max Y cm, yaw
  mean P max |Q| deg, pitch mean R max |S| deg; left to the game: no writer
  match m (of them on last frame's radar target p), no camera rows r, no
  shown call s, no anchor n, over 10 cm o,
  other e; family 8 on a radar target k; writers: 4EDE4BC9 w, 256x256
  5B0068AF v (frames with two or more: z)`. No line at all means the code
  never ran; 0 moved with reasons means it ran and never applied.
- `srv turret radar anchor: correction first applied at frame F, eye E,
  disc|icon: delta (x,y,z) cm in the world, (x,y,z) cm in the eye; the shown
  call was G ms before the camera write`, once.
- `srv turret radar anchor: the setter runs on thread T, the camera writes on
  thread U[ (the same thread)]`, once.
- The 10 s residual line drops `lead now` and gains `shown call before the
  write: mean G min g max h ms`. The direction line drops `led by` and keeps
  the age, the frames, the source and the aim.

To fly: camera back 0 to 35, radar forward = 65 minus camera back,
`fix.srv_turret_radar_anchor = on`. Fast and slow turret sweeps, then drive
forward and turn the vehicle.

### 2026-11-01: the correction flown (flight da716942); the Scorpion's ghost is the disc's depth test

Steam, `edvr_gfx_20261101_044947.log`, build matches, dump
`eye_045244947001` (mid-rotation). Sean: the rotation is still not
perfectly smooth, and a shadow or ghost of the Scorpion trails the real
one while the turret turns.
- **The correction runs.** 4 billboard draws moved a frame (disc and icon,
  both eyes), |delta| mean 0.6 to 1.1 cm, max 2.7 to 5.7 cm. The residual
  now lags a steady 1.00 frame (no lead), and the shown call comes about
  6.5 ms before the camera write. "No writer match" is 2 a frame: the
  512x512 single-target billboard, the SYSTEMS panel's Scorpion (writer
  #6). "Over 10 cm" 4 to 12 and "other" 4 to 12 per 10 s were left to the
  game.
- **Every issue of a moved draw is moved.** The unmoved-draw counts are the
  group-off draws and their one re-issue each (1806 and 1806 in a window,
  equal to "no writer match"); no issue of a moved billboard went out
  unmoved. The hologram depth pass captured nothing at the radar
  (`HoloContribution` and `HoloCoverage` are 0 there in the dump).
- **What the ghost is.** Cropped from both eyes' finals: the orange
  Scorpion sits where it should, and 70 to 85 px to the right is a
  Scorpion-shaped region where the disc's dark fill is missing. There the
  final equals the pre-UI image to the bit (patches 166.8/162.4/162.6 and
  53.3/50.7/50.7 in both), so the disc's draw left nothing at all: no fill
  and no rim. Its edge steps are 2 native px, the game's 2016 pixels, not
  the layer's 4032 and not either picture's texels.
- **Reading.** The only per-game-pixel decision in the billboard's pixel
  shader (EA02FAC2) is its scene-depth test: it loads t1 (the game's
  2016x2017 R32 depth) at its own screen position and discards when t1 is
  more than 0.5 m nearer than the hologram. A hole in the picture (t4) would
  move with the billboard and carry the rim, and this one has neither. So
  t1 holds a Scorpion-shaped near footprint at the place the game drew the
  icon, in screen space. Before the correction the icon covered it; moving
  both billboards by delta leaves it behind, one frame of turret motion
  away. What writes the footprint into t1 is not yet known (the scene
  depth in the dump has no Scorpion). Unconfirmed until it flies.

**The build.**
- **The ghost fix: a far depth at t1.** A moved radar billboard (family 7
  with a t4 match, the draw `radarCorrectionBegin` binds) reads EDVR's own
  far depth at PS t1 for its scope instead of the game's scene depth.
  - The texture is R32_FLOAT at exactly the game's t1 size (read from its
    view), one mip, one slice, one sample, SRV and RTV. It is cleared once
    to 1e9, so the discard (t1 more than 0.5 m nearer than the hologram)
    never fires. One is made per device and size, the first time it is
    needed (`srv_turret_hud_binding.h` `FarDepth`).
  - It passes the crisp layer's depth-source rule (`ui_holo_remap.h`
    `depthSource`, which the rig runs on it on WARP), so the layer still
    takes the draw.
  - The game's t1 is read and held at the bind, ours goes in past the
    binding shadow's hook (as the UI layer's own swaps do), and
    `srvTurretHudEnd` puts the game's back. Every issue in the scope sees
    ours. A game t1 that is not a 2D R32 view, or a far texture that cannot
    be made: the draw still moves, with the game's t1, counted as "t1 kept".
  - Never for family 8, the unmatched 512x512 billboard, or a draw that is
    not moved.
  - The trade-off: under the icon the disc's fill now draws too, where the
    stock footprint cut it, so it is slightly darker inside the Scorpion. In
    the turret view no real scene geometry can be 0.5 m nearer than the
    radar (about 0.45 m from the eye), so no scene occlusion is lost.
- **The pops.**
  - The correction is applied up to 0.5 m, a sanity bound: every draw left
    to the game pops. Frames over 10 cm are applied and counted as "large";
    over 50 cm is left to the game.
  - "other" is split into its causes: delta 0 (no pop), not ready (a
    binding or scissor restore owed, the shader, or the constants), no eye
    frame, params unusable, topology, no game cb, binding begin failed.
  - At most one disc and one icon are moved in each eye each frame; a
    further match is left to the game as "extra radar-target billboard".
- **The writer counter.** Two 256x256 5B0068AF writers a frame are normal
  (one an eye). The line now counts frames with more than two, and the
  first such extra writer is said once at the next frame: draw count,
  instances, target and size, viewport, and whether a billboard's t4
  matched that target that frame.
- **The holo source rounds** keep running while the correction is on
  (`holoCollect` is called from `radarCorrectionBegin`, before the t1 swap,
  so it records the game's t1). The watched range is t1..t7 (was t2..t7),
  so the next round's writer lines name t1's writers, which is where the
  footprint comes from.

Unconfirmed until it flies: that the ghost goes with the far depth; how much
darker the fill under the Scorpion looks; which draws write the footprint
into t1 (the holo rounds' t1 writer lines).

The log lines:
- the 10 s correction line now reads `srv turret radar anchor: 10 s
  correction: N billboard draws moved (eye 0 a, eye 1 b; disc c, icon d)
  over F frames, |delta| mean X max Y cm (large, over 10 cm: L frames), yaw
  mean P max |Q| deg, pitch mean R max |S| deg; t1 far T, t1 kept K; left to
  the game: no writer match m (of them on last frame's radar target l),
  extra radar-target billboard x, no camera rows r, no shown call s, no
  anchor n, over 50 cm o, delta 0 z (no pop), not ready a, no eye frame b,
  params unusable c, topology t, no game cb g, binding begin failed f;
  family 8 on a radar target k; writers: 4EDE4BC9 w, 256x256 5B0068AF v
  (frames with more than two: e)`;
- `srv turret radar anchor: a third 256x256 5B0068AF writer at frame F: draw
  count N, instances I, target ID WxH, viewport (x,y WxH); a billboard's t4
  matched|did not match that target that frame`, once;
- the first-application line ends `; t1 is the far depth` or `; t1 kept
  (...)`;
- `srv turret holo source: round R: no 5559BD94 draw of the frame read a
  render or depth target at t1..t7; nothing to watch` (was t2..t7).

To fly: camera back 0 to 35, radar forward = 65 minus camera back,
`fix.srv_turret_radar_anchor = on`. Fast and slow turret sweeps, then drive
forward and turn the vehicle.

### 2026-11-01: the far depth flown (flight e64866f9); the hole is in the disc's own picture

Steam, `edvr_gfx_20261101_055340.log`, build matches, dump
`eye_055600519001`. Sean: "No change", the ghost is still there.
- **The far depth ran on every moved draw.** The 10 s line read 2932 moved
  (disc 1466, icon 1466), t1 far 2932, t1 kept 0, |delta| mean 0.37 and max
  2.4 cm, and delta 0 28, with nothing else left to the game. So the t1
  scene-depth test is not the hole (ruled out below).
- **t1's writer** (holo rounds 1 and 2) is vs DEF19B035D5EDEDC with ps
  CB95394B50D737D6: a full-screen triangle into the 2016x2017 R32 target,
  which copies the scene depth. The third 256x256 5B0068AF writer (count
  1383, 2 instances, target B4A4E0) was read by no billboard.
- **The stencil names the hole.** In the dump's scene depth-stencil
  (`SceneZ`, the stencil byte), bit 0x04 is set over the whole disc except
  in a Scorpion-shaped patch at the ghost's place. 0x04 is what the radar
  billboards' colourless re-issue writes wherever their pixel shader keeps
  a pixel. So the disc's own draw dropped those pixels in both its issues,
  and with t1 far, the shader's only remaining discard is alpha below
  1e-5: no content in the disc's picture (the centre tap of t4 at 1000 or
  more) and no rim. **The hole is in the disc's picture**, the vehicle
  missing from the terrain. Its 2 px steps are that 512x512 picture's
  texels across the disc, about 1.7 native px each.
- **Why it trails (hypothesis).** The hole moves with the disc's quad, the
  orange Scorpion with the icon's quad. Both quads move by the same delta
  in 3D, but a quad moved in 3D shifts on screen by delta over its OWN
  depth, while its picture shows content at about 0.45 m. The pictures'
  quads need not be at the content's depth: the panel holograms' quads,
  the only 5559BD94 quads the probe has caught, sit 2.4 to 2.6 m out (w).
  If the disc's quad is far and the icon's is near, the disc and its hole
  move about a fifth as far as the icon on screen. That is also a disc that
  is still not smooth. The disc's and the icon's quad depths have not been
  measured.

**The build.**
- **The far depth is reverted.** FarDepth, the t1 swap and its restore, the
  t1 far and kept counters, their rig cases and their pins are gone. The
  holo watch is back to t2..t7 (t1's writer is known: the DEF19B03 depth
  copy). Kept: `holoCollect` on the corrected path, the 0.5 m bound and the
  large count, the split reasons, one disc and one icon an eye a frame.
- **Each billboard moves as its content would.** The shared shader function
  `turretMove` now reads tr.w (padding before, 0 for every draw). When tr.w
  is above 0 a vertex's translation is tr.xyz * (w * tr.w), w its clip w;
  otherwise tr.xyz as before, so every other draw is bit for bit what it was.
  With tr.w = 1 / z_c the moved vertex projects to (x / z + dx / z_c) / (1 +
  dz / z_c), where the content point on the same ray at depth z_c, moved by
  delta, lands, whatever the quad's own depth. The disc, the hole in its
  picture and the icon then shift alike on screen. Signature H's lighting
  line moves l by the vertex's own move, so it stays consistent. The CPU copy
  (`anchoredMove`) does the same, and the constants' test takes tr.w in [0,
  20] (z_c of 5 cm or more).
- **z_c** is the radar picture camera instrument's schematic origin depth
  (V0's `centre[2]`, about 0.34 m). It is used while the instrument has a
  read whose last good read is at most 90 frames old (a new stamp at every
  read, moved or not) and 0.1 m <= z_c <= 2 m. Otherwise tr.w is 0, the plain
  delta, counted as "no content depth". Both eyes and both kinds take the
  same z_c (the icon sits at the schematic's origin). It is set in
  `radarCorrectionBegin` after the translation's constants are made and
  before their test.
- **The rig**, on WARP in every signature's stream-output twin: a quad 2.5 m
  out, a vertex at 0.6 m and one at 0.34 m, with 1 / z_c = 1 / 0.34, land
  where content at 0.34 m on their rays, moved by delta, projects (by the
  camera's own projection). With tr.w = 0 they move by delta at their own
  depth, and a vertex on the eye plane is left alone. On the CPU: the
  freshness and range of z_c, the constants' 0..20, and `anchoredMove`
  against `st::contentShiftNdc` at 0.2, 0.34, 2.5 and 7 m. Three new shader
  mutants (the content depth ignored, scaled without w, scaled at tr.w 0)
  are caught, twenty-five in all. Wiring pins with controls: tr.w set on the
  corrected path only, from the instrument with its freshness and range;
  the shader's and the CPU's tr.w > 0 rule; the quad probe; the far depth
  gone.
- **The instrument.** The corrected path now runs the stream-output twin
  probe, once a second per eye, the disc and the icon in turn. Each sample
  says the quad probe line, beside the probe's usual stats and capture lines
  for family 7.

The log lines:
- new: `srv turret radar anchor: quad probe: eye E disc|icon w=[a,b] pre ndc
  centre (x,y) moved by (dx,dy) ndc; content depth z_c Z m; expected
  (ex,ey) ndc for content at z_c; delta in the eye (x,y,z) cm`. "expected" is
  where content at z_c on the centre's ray goes: moved by equal to expected
  says the quad shifts as its content does, and w says the quad's depth.
  Without a content depth it reads "content depth z_c none (the plain
  delta); expected none".
- the 10 s correction line: "t1 far T, t1 kept K" is now "content depth z_c
  mean X min Y max Z m (no content depth n draws)".
- the first-application line ends "; content depth z_c Z m" or "; content
  depth none (the plain delta)".
- the holo round line says t2..t7 again.

Unconfirmed until it flies: the two quads' depths (the quad probe's w will
show them); z_c as the depth of the picture's content (the schematic's
origin). If the content sits at another depth, moved by will match expected
and the hole will still trail.

### 2026-11-01: the content depth flown (flight 15b659b2); the pair capture and the relief radius (built, not flown)

Steam, `edvr_gfx_20261101_062714.log`, two dumps: `eye_062950164001` (UI
quality off, turret moving) and `eye_063005251002` (UI quality 100, still).
Sean:
- with UI quality off the radar's terrain shows brighter and more visible,
  and he wants the two to match;
- "The whole disc seems to trail the turret motion and does not stay
  perfectly locked to the barrel";
- the Scorpion ghost is still there in the moving dump, on the left of the
  icon this time.

What the flight says:
- **The quad probe refutes the far quad.** Disc w 0.297 to 0.305 m, icon w
  0.315 to 0.323 m, z_c 0.31 to 0.33 m, and "moved by" equals "expected" for
  both: the geometry shader applies what we intend, and both quads already
  sit at the content's depth (ruled out above).
- **The ghost came with the correction.** In flight a8465cb3 (the native
  move alone, no draw-time correction) the dump `eye_213337152001`, taken
  mid-sweep at about 88 deg/s, shows the icon cleanly over the hole. The
  ghost first appeared with da716942. So natively the icon and the disc
  agree within a frame, and the correction separates them by about
  |delta|: the hole sits 70 to 85 px (about 1.6 cm) from the icon, the size
  of delta. One of the two billboards does not get the same move on screen
  as the other, though the counters say both do.
- **Why UI quality darkens the radar.** EA02FAC2's relief comes from an
  8-tap Sobel of t4 at UV +- ddx/ddy(UV) * r, r = max(cb1[120].z, 1) *
  cb2[21].y; the gradient's length * 32 * cb2[20].w (capped by cb2[21].x)
  drives the colour and the alpha. The crisp layer runs the program at the
  layer's density (4032 against the game's 2016 at UI quality 100): the
  derivatives halve, the taps sit half as far apart in UV, and the gradient
  term halves. E95634B0 has the same taps, through r1.w.
- **cb1[120].z is not 1 in VR.** In the scene block of every VR dump from
  `eye_213337152001` to `eye_063005251002` (`EngineNow`, the 5376-byte
  block; its rows 277 and 279 are the camera's unit axes) row 120 reads (0,
  1.8676, 1.3666, 0). So the factor is read from the block, not taken as 1.

**The build.**
- **The relief radius (a fix, inside fix.ui_quality; `ui_holo_remap.h` THE
  RELIEF).** The layer's remapped programs get a second, same-size edit:
  `max r1.x, cb1[120].z, l(1.0)` (EA02FAC2, token 146 of the program;
  E95634B0's `max r1.w` at token 389) becomes `max r1.x, cb13[1].x,
  l(0.0)`, the `mul` by cb2[21].y after it unchanged. b13 is now two
  registers for these programs (the frosted base keeps one), and cb13[1].x
  = k * max(cb1[120].z, 1), k = layer pixels / game pixels (the inverse of
  the position map's scale, the geometric mean of its axes). l(0.0) takes
  that as it is for any k; l(1.0) would floor a layer smaller than the
  game's target at 1. cb1[120].z comes from the scene camera block's Unmap
  tee while the layer is live, kept by buffer and looked up by the draw's
  PS b1; a b1 not seen written takes the latest write of any scene block,
  none at all 1. With UI quality off the game's own program draws, as
  before. No new key.
- **Its rig** (ui_holo_test, on WARP): each program's edit pinned at its
  token index, the frosted base without it, a program with no pair or with
  two refused. At k 1 the edited program is the stock one bit for bit. On a
  linear ramp at t4 the stock program's gradient term is 0.4883 at the
  game's density and 0.2441 at twice it; the edited program at twice it
  gives 0.4883 (with cb1[120].z 0.5: 0.3906, 0.1953, 0.3906), and the
  factor without k leaves it halved.
- **The pair capture (an instrument only; nothing moves differently).** In
  a capture frame each eye's disc and icon billboard both go through the
  stream-output twin, every issue captured, into a ring of six frames read
  back without stalling. Every frame for the 16 frames after an eye dump
  (the dump key), else one frame every 2 s. Each billboard says its t4, its
  VS b1, the forward row (279) its delta was turned with, the setter call
  the frame's correction was paired with (its number, new in the call
  ring, and d), the delta in the eye and z_c, its issues (with our shader
  bound or another), and for each issue the quad's w range, its centre in
  the eye before and after the move, its NDC centre, its move on screen in
  px against content at z_c, and its UV corners. Each eye then compares the
  disc's move with the icon's.
- **The pictures.** In the dump window eye 0's disc t4 (512x512) and icon t4
  (256x256) are copied once each at the bind and written next to the eye
  dump as `eye_<stamp>_RadarDiscT4.bmp` and `eye_<stamp>_RadarIconT4.bmp`:
  black where t4 >= 1000 (empty), gray 64 to 255 for content (brighter is a
  larger t4). The hole should show in the disc's picture; the icon's sits
  in its own.
- **The lock.** In the dump window each eye's post-move disc and icon
  centres (and the pre-move ones) are put in the seated frame with that
  eye's pose at the draw, beside the turret's yaw rate (the desired aim's
  yaw in the anchor's axes, frame to frame). Locked to the turret, the
  seated disc centre stays put through a sweep; trailing, it moves.
- **The issues.** Every moved radar billboard's scope counts its issues and
  the geometry shader bound at each; a radar billboard declined by the
  correction, or a family-7 issue outside any scope whose t4 is a radar
  picture, counts as an issue not moved. A draw moved twice would show as
  a second issue whose pre-move centre is the first's post-move one; one
  not moved, as "another" shader or an issue outside a scope.
- No key: it runs whenever the correction does. The quad probe stands
  aside for a draw the pair capture takes.

The log lines:
- `srv turret radar pair: frame F eye E disc|icon[ (dump S, frame n of
  16)]: t4 ID, VS b1 ID, forward row 279 (x,y,z), setter call #N d (x,y,z)
  cm, delta in the eye (x,y,z) cm, z_c Z m, viewport WxH; issues I (our
  shader O, another A), prims P (needed Q); issue 0: w=[a,b], centre in
  the eye pre (x,y,z) post (x,y,z) m, ndc pre (x,y), moved (x,y) px,
  expected (x,y) px, uv (u,v) ...; issue k: pre (x,y) px from issue 0's,
  moved (x,y) px, w=[a,b]`. Pixels are the game's target's, x right, y up.
- `srv turret radar pair: frame F eye E: disc moved (x,y) px (expected
  ..), icon moved (x,y) px (expected ..); disc minus icon (x,y) px, |D| px[
  -- APART]` (APART: over 1 px). If only one was captured: `... disc
  measured|lost|not captured, icon ...; nothing to compare`.
- `srv turret radar pair: lock: frame F eye E (dump S, frame n of 16): disc
  centre seated post (x,y,z) pre (x,y,z) m, icon post (..) pre (..) m;
  turret yaw rate R deg/s`, in the dump window only.
- `srv turret radar picture: dump S frame F eye 0 disc|icon: t4 ID WxH
  format N, content (t4 < 1000) C of T texels, t4 a to b; written
  eye_S_RadarDiscT4.bmp ...`, once per picture per dump (or why not).
- `srv turret radar pair: dump S window ended: N of 16 frames captured (eye
  0 disc a, icon b; eye 1 disc c, icon d); pictures: disc ..., icon ...;
  radar billboard draws seen R`, once per dump while the correction ran.
- `srv turret radar pair: 10 s: disc: draws moved A, issues moved B, issues
  not moved C (another shader in the scope c1, declined draws c2, outside a
  scope c3), issues per moved draw 0/1/2/3+: ..; icon: ...; disc and icon
  moved apart on screen (over 1 px): by the constants X of F eye frames (no
  content depth n), by the capture Y of Z captured eye frames; capture
  frames N, skipped (ring busy) S, entries lost L`, with the 10 s
  correction line. "By the constants" compares the delta and z_c each was
  given; "by the capture", the measured moves.
- `crisp holo remap: relief radius applied to PS P (VS V): k K (layer WxH
  over the game's WxH), cb1[120].z Z from <source>, radius factor F = k x
  max(z, 1)`, once per program; a later fallback source is said once.
- No pair line at all with the correction line present means the
  instrument never ran; "capture frames 0" means it ran and took nothing.

To fly: radar anchor on, camera back 0 to 35, radar forward = 65 minus
camera back, UI quality 100. A fast turret sweep with INSERT pressed during
it; then the turret still, INSERT at UI quality 100 and again at off.

Unconfirmed until it flies: that the hologram draw's PS b1 is the scene
block the tee sees (the relief line names its source); that the relief fix
brings the fill to the UI-quality-off look; which billboard's on-screen move
differs, and whether the hole moves with the disc's quad or inside its
picture.

### 2026-11-01: the pair capture flown (flight 8e3bcd7b); the lock (built, not flown)

Steam, `edvr_gfx_20261101_072456.log`, dump `072742058001`.
- **The relief radius works.** "relief radius applied ... k 2.0000 ...
  cb1[120].z 1.3666 from the bound b1's last write, radius factor 2.7332"
  for both programs.
- **The disc's picture has no hole.** `eye_072742058001_RadarDiscT4.bmp` is
  a full ellipse. Still frames show no ghost; only sweeps do (the hole idea
  is ruled out above).
- **The lock lines name the cause: the game's own placement lags the
  turret by far more than a frame, the disc and the icon by different
  amounts.**
  - At rest: disc seated post (0.007, -0.220, -0.055), icon post (0.005,
    -0.212, -0.076).
  - At a steady -130 deg/s: the disc's pre-move x about +0.09 m (7 to 9 cm,
    about 65 ms of lag), the icon's about +0.07. The delta takes off only
    the setter's one frame of it, so the disc's post-move x is still +0.053
    and the icon's +0.031: the trailing Sean sees, and the 2 cm between the
    two is the ghost.
  - At -344 deg/s even the eyes differ: disc pre x 0.151 in eye 0 and 0.238
    in eye 1, in the same frame.
- **Reading.** The game damps the radar's world placement toward P + d over
  several frames, differently for the terrain disc and the icon. In the
  stock game this cannot show, because P sits on the yaw axis; our d
  rotates with the turret, so the damping does (ruled out above:
  correcting by the setter's last d).

**The build: the lock (srv_turret_math.h and srv_turret_hud_shader.h THE
LOCK).**
- **In the geometry shader** (all eight share the code; only the radar
  correction sets the flag, TurretParams::lock, the anchor register's w):
  each triangle's three eye-space points are rebuilt from the game's clip
  positions, C is the midpoint of the triangle's longest edge, and every
  vertex moves by T - C, T the billboard's target in the eye (where the
  delta went). A quad's two triangles share the diagonal, each one's
  longest edge for a rectangle, so both take the same C (bit for bit) and no
  crack opens. The game's NDC depth is kept, and signature H's lighting
  position moves by the same vector.
- **Single quads only.** The draw's count and instances (noted by
  vscreen.cpp before the pass's scope) must be a list of 6 or a strip of 4,
  one instance; any other draw keeps the delta, counted.
- **The targets, on the CPU, one per kind, in the seated frame.**
  - **Rest:** the turret's yaw and pitch rates on the vehicle (the desired
    aim in the anchor's axes, frame to frame) and the vehicle's own turn
    rate (the anchor's world rotation, frame to frame) all under 3 deg/s,
    20 frames in a row. Pitch is in the rest test too: an elevation change
    moves d as a yaw does. Linear motion is not (the call ring carries no
    translation), so driving straight counts as rest.
  - **Learned value:** the mean of the pair capture's pre-move seated
    centres at rest (both eyes of a frame averaged), first after 6 samples;
    while a kind has none, the capture runs every 5 frames (silently),
    else every 2 s. After that each rest sample moves it by 1 - exp(-dt /
    1 s), dt the time since the last sample, at most 2 s.
  - **Reset:** when the view comes on, and when camera back, radar forward
    or the radar anchor key changes.
  - **Before a target exists:** the delta, as before.
  - **At the draw:** the target is put in the eye with that eye's pose of
    the draw (the inverse of the lock line's conversion).
- **Unchanged:** the setter's offset and its no-lead rule, the pair
  capture, the lock lines, the relief radius.
- **The rig** (srv_turret_hud_test, WARP, all eight signatures): a
  rectangle and a sheared, depth-tilted parallelogram (cut along its long
  diagonal) land with each triangle's centre on T to 1e-5 m, the shared
  corners bit for bit, every vertex moved by T - C with the game's depth,
  and H's lighting position by the same vector; with the flag at 0 the
  twin is the plain translation. Six lock mutants are caught. On the CPU:
  the draw-shape rule, seated to eye as the inverse of eye to seated, the
  vehicle's turn angle, rest (each rate, a missing frame), the learning
  (the sixth sample, the 1 s average, the 2 s cap, a non-finite sample) and
  the reset rules. A wiring pin with five controls.

The log lines:
- `srv turret radar lock: disc|icon target learned at frame F: seated
  (x,y,z) m, the mean of N rest samples (eye 0 (..) m from n, eye 1 (..) m
  from n); its billboard is now locked to it`, once per kind (and after a
  reset).
- `srv turret radar lock: targets reset (the turret view came on | camera
  back, radar forward or the radar anchor key changed)[, none had been
  learned]; each kind keeps the delta until it is learned again`.
- `srv turret radar lock: 10 s: draws locked L, on the delta D (no target
  yet a, not a single quad b, no eye pose c, target too far e); |T - C| (the
  shift a captured locked draw was given) mean X max Y cm, its centre off
  the target mean .. max .. mm, over N captured draws; targets: disc
  seated (..) m | none (k of 6 rest samples), icon ...; reset R, re-learned
  S samples; frames at rest F of G`, beside the correction line.
- Each pair billboard line ends `; LOCKED: target in the eye (x,y,z) m,
  applied shift (x,y,z) cm, centre post minus target (x,y,z) mm` or `;
  delta mode (why)`.
- The pair lock line (dump window) adds `turret yaw rate R deg/s, at rest |
  not at rest; disc target seated (x,y,z) m, locked | why, applied shift in
  the eye (x,y,z) cm, post minus target (x,y,z) mm; icon ...`. Locked, the
  post-move seated centre should stay at the target through a sweep.

To fly: radar anchor on, camera back 0 to 35, radar forward = 65 minus
camera back. Sit still for a second (both targets learned), then a fast
and a slow turret sweep with INSERT during each; then drive forward and
turn the vehicle.

Unconfirmed until it flies: that rest comes (the 10 s line's frames at
rest); that both kinds lock (locked against delta); that the post-move
centre stays at the target through a sweep; whether driving or turning the
vehicle shows the same damping (the targets do not follow a moving vehicle
until it rests).

### 2026-11-01: the lock flown (flight fe0b2560); the radar's own camera moved back

Steam, `edvr_gfx_20261101_080045.log`. The lock held (centre off the
target 0.00 mm, shifts 2 to 8 cm, max 25 cm), but Sean says the disc now
"sort of orbits up and down". The V0 decode in the same log shows the
native content origin swinging +-9 cm sideways in the camera frame during
sweeps: the content really does lag. A pinned quad holds a picture drawn
from a viewpoint 10 to 25 cm wrong, so its shape and its place inside the
quad wobble (ruled out above).

**Reading across 8e3bcd7b and fe0b2560.** Any turret-dependent native
offset (our d = F u in the setter) reaches the screen several frames late,
and differently for the disc and the icon. Either the setter's thread
(29912) runs frames ahead of the camera writes (thread 14084), so the
"shown call" pairing was never right, or the game damps the placement
after the setter. The cure is the same either way: no turret-dependent
native offset at all.

**The design.**
- The radar stays where the game puts it (the setter adds nothing). It sits
  on the turret's yaw axis about 0.44 m behind the stock eye and does not
  swing with the turret, so pipelining or damping has nothing to show.
- The radar's schematic slots (the terrain disc and the Scorpion icon, both
  eyes) are given the scene camera moved back by D, the radar forward key,
  along the turret's seated back direction. The volume is then in front of
  that camera, so the gate passes, V0 tracks, and the game frames, renders
  and billboards the picture from E - D: exactly what the eye would see of
  the radar moved forward by D. Panel holograms and every other slot keep
  the real camera.
- The geometry shader moves the radar's billboards by +D along the seat,
  the translation the radar group already gives DF3503CD and the contacts.
  No delta, no lock.

**The RE (build 334925, read-only; inbox item
`20261101-082718-srv-radar-slot-camera`, waiting for intake).**
- 0x36BF3C0(slot in RCX, camera in RDX) builds the gate, the window, the
  billboard's quad corners (slot+0x380..0x3FF) and V0 (a value copy into
  *(slot+0x100)+0x20) from that camera alone, and keeps no pointer to it.
  Its only caller is 0x36BF330; in VR, list A takes the left eye's camera
  and list B (mirror slots, slot+0x288 = 2) the right eye's.
- The camera object (0x2F0 bytes): world matrix at +0x00 (rows x, y, z;
  float position at +0x30), view at +0x170, projection at +0x1B0, VP at
  +0x1F0, dirty flags at +0x230 (bit 3 makes 0x4F4AF0 rebuild VP from view
  and projection), near +0x234, far +0x238. No inverse is cached. Moving it
  by a world d takes the position, view row 3 (minus d through the view's
  axis rows) and the VP-dirty bit.
- A radar slot is told from its own data: the slot's class vtable
  0x54FB7F8, its index (slot+0x258) and manager (slot+0x280) name a
  component list in which some component links (+0x388) to the radar
  interface (vtable 0x5511F18); that component's mode (4 terrain, 5 icon)
  says which.
- The thread is unconfirmed (0x287B050 runs from the engine's update
  dispatch); the camera objects are members of persistent objects.

**The build.**
- **The mode.** `fix.srv_turret_radar_anchor` takes a TEMPORARY fourth value,
  `camera`. `on` (and reversed) keep the setter's move, the delta and the
  lock; `off` is off. In camera mode the setter publishes a zero offset, so
  the existing restore gives the game its own placement back, and there is
  no anchor yield: no delta, no lock, no content depth.
- **The slot hook** (`srv_radar_anchor.cpp`), on 0x36BF3C0, is pinned like the
  setter's: PE stamp 0x6ADA8048 and size 0x640C000, and 21 prologue bytes
  `40 55 41 56 41 57 48 8D AC 24 10 F9 FF FF 48 81 EC F0 07 00 00`, checked
  against the exe and verified before the patch. CodeHook steals the three
  pushes (6 bytes; its patch is 5), not the 14 bytes the brief named. Its
  decoder already took all five encodings; a vtable_test cell pins them.
  It is armed once, the first frame the view is on in camera mode, and
  stands down with nothing patched on any mismatch. The audit's
  exact-build entry for the file now names the slot camera too.
- **Its body** (game thread: records only, every game read under SEH):
  - It forwards unchanged unless D > 0 and `st::radarSlotTest` says the
    slot is the radar's: the RE's test, a read failure counted as a fault.
  - It copies the 0x2F0 bytes into an aligned stack buffer and rebases a
    plane pointer that points into either of the source's inline blocks.
  - It moves the copy by d = -D u (`st::cameraMoveBy`): position += d, view
    row 3 -= d through the view's rows, flags |= 8. Then the original runs
    with RDX = the copy.
  - u is the CPU camera sample's turret forward while its write is at most
    100 ms old, else the camera's own forward (world row 2, made unit).
  - Both lists get the same world d. D is the radar forward key in metres,
    capped at 1.5 m like the setter's F, published every frame (0 outside
    the view or camera mode).
- **The draw path.** In camera mode the writer watch stays on. A family-7
  draw whose t4 is this frame's radar writer's target goes through the
  regular radar path (the group's params with tRadar, tr.w 0), the same
  translation DF3503CD and the contacts get. Every other 5559BD94 draw
  (family 8, the panel holograms) is the game's own, as in on mode, and the
  holo source rounds still see it.
- **The rig.**
  - The mode's spelling and name, the zero setter offset, and D from the
    key.
  - `cameraMoveBy` on a camera whose view is the inverse of its world: view'
    equals an independent inverse of world' to 1e-6, nothing else changes,
    and d = -D times forward grows view row 3's z by exactly D.
  - The rebase inside, outside and at the edges of both blocks.
  - The radar-slot test over a fake memory: a hit (icon, list B), the wrong
    slot vtable, an index past the lists, no manager, more than 64
    components, no link, the link's vtable wrong, a fault, and the terrain
    in list A.
  - The direction rule: fresh, stale, missing, future-dated and zero
    samples, the camera fallback, and D of 0, negative or NaN.
  - Wiring pins with 11 controls for the zero offset and no yield, the build
    pin and prologue, a quiet hook, the copy only for a radar slot, the
    rebase, VP dirty, D published and the regular path.

The log lines (frame tick):
- `srv turret radar camera: slot hook armed: EliteDangerous64.exe+0x36BF3C0
  (the schematic slot's update, build 334925) at 0x.., stolen=6 bytes,
  prologue 21/21 bytes verified, relay at 0x..; the radar's slots get a
  camera moved back by the radar forward key`, or `... slot hook stood down:
  <why>; nothing was patched`.
- `srv turret radar camera: moved back D m for slot 0x.. (index i,
  terrain|icon (mode m), list A|B (l)), direction from the turret (CPU camera
  sample)|the camera's own forward, u (x,y,z)`, once per kind and list.
- `srv turret radar camera: 10 s: slot calls N, radar slots M (terrain A a,
  terrain B b, icon A c, icon B d, other o), moved K (direction turret t,
  camera forward f), faults x; hook thread T, camera writes on thread W`,
  whenever camera mode was on with the view in the window. It ends "-- NO
  radar slot in these 10 s" when M is 0, or names an unarmed hook. Absent
  otherwise.
- The settings line's radar anchor reads `camera`; `edvr_log.py`'s settings
  pattern takes it.
- What should follow: the V0 instrument's lines (`srv turret radar camera:
  ... the schematic's origin at (x,y,z) m`) should show the origin about
  D - 0.44 m in front of V0 (z about +0.2 at D 0.65), tracking every frame
  instead of freezing.

Unconfirmed until it flies: the hook's thread against the camera writes'
(the 10 s line names both); which slot indices the terrain and the icon
use; that list A and list B are the two eyes; the camera's +z as forward
(the fallback only); and whether the billboard quad the game builds from
the moved camera, moved +D by the shader, lands where the eye expects.

### 2026-11-01: camera mode flown OK (build e443b0c2); the nine keys become experimental

Flown on Steam, build e443b0c2, with `srv_turret_radar_anchor = camera`: the
radar stays locked to the turret with no ghost, and the UI relief parity fix
(the layer's relief radius, 8e3bcd7b) works. Sean's word: demote the Scorpion
turret settings to experimental and bake them as defaults.

- **The move.** The nine keys left `[fix]` for the end of `[experimental]` in
  `edvr.ini` (a new heading there, "The SRV's turret view"). They lose their
  menu rows: the settings generator refuses a `menu` token on an
  `[experimental]` key, so the `ui:` lines stay for the labels, the choices
  and `recommended`, and nothing else. The module, the rig's wiring pins and
  `edvr_log.py` name them `experimental.srv_turret_*` now.
- **The defaults** are the values he flew with (the settings line in the log
  prints them):

  | key | default now | was |
  |---|---|---|
  | `srv_turret_camera_back` | 25 | 0 |
  | `srv_turret_hud_forward` | 65 | 70 |
  | `srv_turret_radar_forward` | 50 | 70 |
  | `srv_turret_panels_back` | 135 | 120 |
  | `srv_turret_radar_back` | 0 | 0 |
  | `srv_turret_hud_gap` | 0 | 0 |
  | `srv_turret_hud_depth` | game | front |
  | `srv_turret_hud_scissor` | open | open |
  | `srv_turret_radar_anchor` | camera | on |

- **Side effects.** Text that is not one of the anchor key's words reads as
  the default, camera (it read as `on`); the rig's pins follow. The rig's
  geometry cases keep a push of 1.2 m as a fixture of their own, no longer
  the panels key's default. There is no `moved-from` line: an old `[fix]`
  line carries the old shipped defaults of every earlier install, which would
  undo the bake, so it is not read. Two fixtures in `edvr_log.py` (`old_set`,
  `old_set4`) still print `fix.srv_turret_*`, because they are the settings
  lines older builds wrote.
- **Still to do** when the arc closes: ship the values as the behaviour and
  propose removing the nine keys, quoted, as Scope control requires.

### 2026-11-01: the strip (the instruments and the dead modes out; camera mode's code path unchanged)

Sean's ask: take the TEMPORARY instruments out of main, keep what flew OK (camera mode, the panels' move, the relief fix, the nine keys at their defaults), and say what is left to propose. Nothing was flown; the build is the check.

- **Removed, by group.** The GPU instrument (the probes, the pipeline and stream-output queries, the stats and capture lines, the disc and quad probes) and its stream-output twins, which had no other user in the module. The holo source (the watched rounds, the writers, the constants lines). The radar camera V0 instrument. The unmoved-draw instrument (its routes, the reissue sites, the first-of-route lines). The pair capture and its pictures and eye-dump hook. The seated lock. The draw-time correction, its content depth and the third-writer note. The residual and the setter's 10 s lines, the direction line, the thread and suspect lines. The update counter (0x384A590): it wrote nothing and camera mode does not need it. Dead math in `srv_turret_math.h` (50 names with their comments); the rig's cases for them; `copyPsRegister` (the probe's only copy).
- **Kept, and why.** The slot hook (camera mode). The CPU camera-sample path for u (`srvTurretNoteCameraWrite`, the Unmap tee), with the GPU readback kept as its fallback, since a miss still needs a u. The radar writers and `billboardKind`: camera mode uses them to tell the radar's own picture from the other 5559BD94 draws. The setter hook, for `on` and `reversed` (dead, but their values stay until Sean says). The first-scope state note (not on the TEMPORARY list). `g_cameraWriteThread`, which the camera 10 s line prints.
- **Changed on purpose.** The setter hook is armed only for `on` and `reversed`; camera mode does not patch it (it published a zero offset, so the game's placement is unchanged). The writer watch is on in camera mode only (its yield term went with the correction). In `on` and `reversed`, radar billboards are left to the game (no correction), so the disc orbits as before.
- **Rig and log rule.** `tools\srv_turret_hud_test` keeps the move, linkage, binding, scissor, constants, strips, latch, slot camera, and eye-pose checks, and gets a copy of the stream-output declaration its own twins use (the move test needs it). Its wiring pins follow the new source. `tools\edvr_log.py` drops the GPU-instrument and unmoved verdicts; the 10 s line regex follows the trimmed line.
- **Verified.** `build.bat --wait-lock` by absolute path exited 0 with a receipt; the SRV rig printed `PASS srv_turret_hud_test: 40417 checks` (the move, linkage, binding and mutants on WARP). `edvr_log.py --self-test` and `hook_signatures.py --self-test` pass. Not verified: any flight. The camera path is checked by reading and by the rig's slot and move cases; `on` and `reversed` changed (no correction) and are unflown.
- **Environment note.** A worktree made by the app had no `third_party\openxr\loader`. Linking that folder from the main checkout (junction) gave the pinned loader; `fetch_openxr_loader.py --verify` passes. Remove the junction with `rmdir` before the worktree is cleaned up.
- **Proposals for Sean (none removed):** the `on` and `reversed` values with the setter hook; the nine keys at arc close; `srv_turret_hud_depth`'s `game`/`real` and the scissor's `game`; the TURRETPRE output in the shader (its own build and flight); the GPU fallback for u, if the flights show the CPU path always wins; and the "TEMPORARY" word in the settings line.
- **Still to do:** the flight of the stripped build; the removal decisions above.
