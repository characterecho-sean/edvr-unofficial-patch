# EDVR 0.22.0-rc.1

## Status

RC1 is a prerelease for testing native on-foot VR.

## Native on-foot VR

Native on-foot VR owes its start to [pixel-565](https://github.com/pixel-565)
and their initial investigation. Without that research, it would still be a
dream.

Odyssey's first-person world now renders in stereo from your commander's eyes.
You can walk, use tools and fight in VR, with your commander's body beneath
you. This replaces Explorer Cam and its free-camera switch. Native on-foot VR
is on by default; open F8 and choose the Native on foot page to change it.

The page groups the controls into look and aim, aiming down sights, body and
view, and the HUD. You can adjust eye height, weapon placement and the HUD's
size, distance and position while playing.

- Look to aim is optional and off by default. Turn it on to face, walk and aim
  where you look, including up and down. The mouse or right stick turns you in
  45-degree snap steps by default; you can change the step or choose smooth
  turning. With Look to aim off, the mouse and stick keep the game's usual look
  controls while your head looks around independently.
- Aiming down sights follows your head by default. "ADS: sight in both eyes"
  shows the aiming eye's scene to both eyes so the sight sits centred in each.
  This flattens stereoscopic depth across the shared view while you aim; the
  outer edge that the aiming eye cannot see keeps the left eye's own view. Turn
  that option off to retain normal stereo. You can also turn head aiming off
  and adjust how close the sights sit to your eyes.
- The visor is raised in breathable air by default. Helmet effects such as
  breath fog are centred on each eye's optical axis. That correction runs
  automatically and keeps the original draw when eye identity cannot be
  trusted.
- Terminals hide the on-foot HUD while their interface fills the view, and
  their changing text is held to the current frame after upscaling. Boarding
  clears the on-foot camera adjustments and restores your stored cockpit seat
  position.

Native on-foot VR currently requires Elite build 334925. It stands down on
other builds. The old Explorer Cam settings and hotkey are ignored; F5 no
longer switches to Explorer Cam. Older configuration files remain readable.

## RC1 testing notes

The full on-foot training was completed on Epic build 334925 with a Pimax
Crystal Super at 90 Hz through Pimax OpenXR, using DLSS preset K at 2016x2017
input and 4032x4034 output per eye. The pilot reported that it looked good.
This run covered normal movement, weapons, aiming, snap turns and the return to
the cockpit.

The on-foot mouse cursor is hidden by default because its visible position can
drift from the panel's click position in VR. Mouse clicks still work. The
advanced `fix.native_on_foot_panel_mouse = on` setting draws the cursor again;
it does not resolve that alignment problem.

Leaning too far from your rest position or into world geometry fades the view
and can recentre your position while preserving your facing direction. Normal
training stayed inside those limits. Deliberate wall contact, automatic
recentring and recovery from tracking loss still need focused headset testing.

## Other changes

- Game hooks can now find their targets by code signatures after an Elite
  update, and stand down individually when the required targets cannot be
  verified. Native on-foot VR and the Oculus route profile still require the
  exact supported build. This has been checked against earlier game binaries;
  compatibility with a future update remains untested.
- In the flat profile, Galaxy and System maps opened on foot now get the
  map-specific rendering path, avoiding the temporal ghosting caused by
  treating them as ordinary gameplay.
- Removed unused GPU timing queries from the flat UI path. They were doing work
  whose results were discarded, with a reported performance cost on DXMT.
- Reduced the work needed to identify old motion-data stamps without changing
  the result.
- Normal on-foot logs now summarise measurements every ten seconds while
  recording events and errors as they happen. Detailed logging remains
  available through `log.on_foot_detail = on`. The training run averaged about
  0.43 MB per minute on foot with detail off.
- Fixed a diagnostic eye-capture crash caused by unloading the shader compiler
  while its compiled blobs were still in use.
- The command-line install tool now archives the installed game executable
  locally before installing, preserving a copy for investigating later game
  updates.

## Thanks

Special thanks to [pixel-565](https://github.com/pixel-565) for the
foundational investigation into native on-foot VR. Their research gave this
work its starting point, and this release owes a great deal to it.

Thanks to @dmnemec for the game-executable archive tool and game binary map in
PR #121.

Thanks to @RocketsProjects for the closed-form stale-stamp calculation, and to
@Plokhi for finding the unused flat UI GPU timers and their cost on DXMT.

Thanks as well to everyone who opened an issue or sent logs for this release.
Those reports are how the fixes above were found and checked.
