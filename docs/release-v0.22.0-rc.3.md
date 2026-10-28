# EDVR 0.22.0-rc.3

## Status

Release notes for `v0.22.0-rc.3`, the third 0.22.0 prerelease. Changes below
are relative to RC2. Native on-foot VR requires Elite build 334925.

## Native on-foot VR: looking and moving

Native on-foot VR began with [pixel-565](https://github.com/pixel-565) and
their initial investigation. Without that research, it would still be a dream.
RC3 continues that work with fixes from RC2 reports and further testing.

- Look to aim no longer walks the view upward and then switches itself off in
  station and ship lifts, or after climbing out of the SRV.
- Look to aim now pauses while an on-foot panel, the inventory or the Esc menu
  is open, so they no longer move or judder with your head. It resumes centred
  on your head when they close.
- After a long frame stall, Look to aim no longer leaves your body facing about
  40 degrees away from your head.
- With "ADS: aim with head" off, aiming down sights no longer judders. Leaving
  pistol ADS no longer leaves the view shifted to the right, and the view at
  the hip now sits straight ahead of you instead of a few centimetres to one
  side.
- With Look to aim off, looking down with the mouse now moves your view as a
  neck would, instead of showing your torso bent far below you. Looking up
  unarmed no longer puts the view inside your torso, or makes it jump when you
  look level again.
- The weapon readout no longer starts following the headset for the rest of the
  session after you have been near an SRV.

## Aiming down sights

- ADS now puts your eye on each weapon's own sight line, the one the game uses
  in flat, so the sight and the shots agree. This fixes sights that sat above
  or below the point of aim on several weapons, and the laser SMG, whose sight
  did not line up at all.
- The view no longer pulls to one side while the weapon comes up to the sights.
- How close your eye sits to the sights now follows the game's own aiming
  camera for each weapon. The kinetic assault rifle and the laser SMG sit
  further back, so you no longer see inside the gun. Carrying the same weapon
  twice, scoped and unscoped, no longer makes the view jump between the two.
- Leaving ADS now returns the view smoothly, without the pause and the jump
  upward at the end. Holstering a weapon or tool now moves the view to the
  unarmed pose as the weapon goes away, instead of one to three seconds later.

## Weapon readouts

- Each weapon's ammunition readout now has its own position at the hip and in
  ADS, replacing the shared pistol and rifle positions. The plasma pistol and
  the scoped plasma sniper are placed too.
- The readout no longer flickers in either eye while you move your head.
- The readout stays on the weapon with anti-aliasing off, and after
  anti-aliasing is switched off and back on. Before, it rode with the headset.

## Body and outfits

- Suits with an overcoat cosmetic now show the coat on your body. Your arms no
  longer float apart from the shoulders, and the coat's first-person sleeves no
  longer appear as a second pair of arms.
- The coat suit's backpack and antenna are hidden from your view, including
  with a rifle drawn and when you look over your shoulder.
- The left elbow no longer sometimes rises to shoulder height with two-handed
  weapons such as the kinetic assault rifle, the SMGs, the plasma sniper and
  the rocket launcher.
- Hands stay on a weapon or tool after the game recreates your character during
  a session, instead of trailing behind the grip.

## On-foot panels and boarding

- The mouse cursor on on-foot panels such as lockers and the inventory is now
  drawn in VR where your clicks land. It follows the mouse and stays on its row
  as you turn your head. This resolves the cursor limitation listed in RC2.
- Boarding your ship or SRV now restores your stored seat after the movement
  leash has recentred you on foot. Before, the seated view could stay where you
  last stood, about 37 cm in front of the seat (#135).

## Visual options

- The headset's F8 menu has a new Visual page. Screen curve, Intro background
  picture and Intro movie moved there from the Fixes page, beside the two
  options below.
- World scale (`visual.world_scale`, VR only, live) sets how big the world
  looks, from 50 to 200 percent. Above 100 the world looks bigger, below 100
  smaller. It changes only the distance between the two eyes the game renders
  with; head movement and field of view are unchanged. The numbers match OpenXR
  Toolkit's and SteamVR's world scale.
- Star glow (`visual.star_glow`, live) now works in VR, with the stronger halo
  in each eye. It strengthens the halo Elite's bloom draws around stars, from
  100 percent (the game as it ships) to 200 percent. It needs Bloom on in
  Elite's graphics options, and both menus now say so when Bloom is off.

## Settings changes

- Retired: `fix.native_on_foot_ads_eye_distance_cm` (the "ADS eye distance"
  row; each weapon now has its own eye position) and
  `fix.native_on_foot_panel_mouse` (the cursor is always drawn now). A saved
  value for either is ignored and noted once in the log.

## Performance and diagnostics

- In VR with anti-aliasing on, EDVR now spends about 0.2 ms less CPU time per
  frame at a busy settlement pad, with identical output.
- Flat diagnostic captures each get their own folder, so two captures started
  in the same millisecond no longer mix their files (#105).
- The pull request reply tool now refuses to post while a pull request has
  activity it has not seen (#96), and the build checks that every tool keeps
  its self-test. Fixed a race in the flat owner-wait diagnostic line that made
  a build check fail now and then (#138).

## Remaining limitations

Scopes that zoom in the flat game do not magnify in VR yet. A design for scope
magnification exists.

On some spawns the coat suit's backpack can show for a few seconds after you go
on foot, most likely while the game is still loading the full-detail model.

With Look to aim on, the aim has been seen drifting away from the head on a
planet surface. This is still being investigated.

The mission-completion menu still uses the game's curved panel and can clip
through the terminal.

Quitting the game can leave the headset dark for several seconds while the
desktop window keeps moving. That delay is in the game's own shutdown, after
EDVR has shut down.

## Thanks

Thanks to @dmnemec for unique flat capture folders in PR #113, the pull request
reply tool's check for unseen activity in PR #107, and the build check for the
tools' conventions in PR #129.

Thanks as well to everyone who opened an issue or sent logs for this release.
Those reports are how the fixes above were found and checked.
