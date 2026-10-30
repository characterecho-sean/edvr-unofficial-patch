# SRV turret view: camera position and HUD depth

## Status

- **State (2026-10-30):** BUILT on branch `claude/srv-turret-hud` (ab45d50b,
  full build green, rig `srv_turret_hud_test` 487 checks), NOT FLOWN.
  Issue #140. Test install: Steam (`--target steam --scope both --xess`).
- **What is wrong (measured, the game's own placement):** in the Scorpion's
  turret view Elite puts the VR camera too far forward along the gun. The
  near HUD layer (both weapon arc brackets, SECONDARY A / PRIMARY A, the two
  ammo counts) sits about 0.11 m behind the default eye point, so it is
  behind the player's head. The target info panel and the SYSTEMS panel sit
  about 1.02 m deeper than that layer, but are drawn over it, so in stereo
  the far panel covers the nearer arcs. The reticle is at the target, as it
  should be.
- **What the build does (Sean's plan, 2026-10-30):** in the turret view only
  (Status.json Flags bit 13, seated in an SRV), (1) the eye alignment moves
  the camera back `fix.srv_turret_camera_back` cm (default 60), and (2) a
  private geometry shader (`srv_turret_hud_shader.h`) brings the targeting
  sprites (vs E508648660A352B2) that lie between gap + 0.1 m and 4 m forward
  by `fix.srv_turret_hud_gap` cm (default 102), along the ray from the head
  centre, so they keep their size on screen and land at the ammo count's
  depth. The reticle (far) is outside that window and stays at the target.
- **Temporary keys** (live, Fixes page of the menu; bake the flown values and
  propose removing both when the arc closes): `fix.srv_turret_camera_back`,
  `fix.srv_turret_hud_gap`.
- **Latency:** both follow Status.json, which the watcher polls every 500 ms
  and the game rewrites about once a second, so a view switch is followed up
  to about 0.5 s after the game writes it. The on-foot eager poll
  (`journalWatchSetEagerStatusOnFoot`) never shortens the worker's sleep:
  `untilNextDue()` honours `g_eager` only. Left alone; a fix would change on
  foot too.
- **Open:** whether the radar (bottom centre) is also at the deeper depth
  (it was outside the dump's view); whether the Scarab's turret view has the
  same placement; whether the switch lag is acceptable.
- **Ruled out:** see "Ruled out" below.
- **Next flight:** Steam, VR. Board the Scorpion, enter the turret view
  sitting normally, look at the HUD, press the eye dump (NumLock) once
  sitting still, leave the turret view. Then `python tools\edvr_log.py
  --target steam --expect-build HEAD --health` (rule "srv turret view") and
  `--grep "srv turret"`: the 10 s line should read P00 near 0.94, ipd_elite
  near 55 mm and bound equal to draws. Measure the dump with the disparity
  method below.

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
strip. The info panel, the SYSTEMS panel and the reticle are three draws of
vs E508648660A352B2 (one quad mesh, three instance records, a 4512x3948
surface), drawn after the panels. That shader writes `o1.z = |o1.w|`, the
near plane in Elite's reversed Z, so it covers everything regardless of its
depth: this is why the deeper panel hides the nearer arcs. Both shaders build
a camera-relative position and project it with `dp4 cb0[4..7]` (the rows'
translation is zero), so clip w is the depth along the eye's forward axis.

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
- ruled out: a single per-draw constant for the sprites, because the three
  sprite draws share one shader and mesh and include the reticle, which must
  stay at the target: only a per-vertex depth window separates them.

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
