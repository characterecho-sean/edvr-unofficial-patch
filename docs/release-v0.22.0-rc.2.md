# EDVR 0.22.0-rc.2

## Status

Release notes for `v0.22.0-rc.2`, the second 0.22.0 prerelease. Changes below
are relative to RC1. Native on-foot VR requires Elite build 334925.

## Native on-foot VR fixes

Native on-foot VR began with [pixel-565](https://github.com/pixel-565) and
their initial investigation. Without that research, it would still be a dream.
RC2 builds on that work with fixes from the first round of testing.

- Restored the visible body's lower-arm bracers while hiding the duplicate
  first-person pieces that detached from the arms.
- Made sniper aiming down sights more reliable when the game accepts an aim
  request late. Releasing ADS after an automatic position recentre now keeps
  the original facing direction instead of leaving the view turned.
- Fixed pistol sights sitting too low while aiming: the standing eye-height
  adjustment now releases while the sights control the view.
- Fixed a laser-rifle aiming fallback that could leave the sight to the left of
  the view when its pose samples arrived one frame apart.
- Ship selection now uses the ship-preview camera for the rendered eyes, rather
  than leaving the view inside the selected ship. The preview stays at the
  game's viewing pose while the menu is open.
- Sitting follows the seated body instead of holding the standing eye height.
  The view keeps that seated height while the body remains seated, avoiding the
  floating head and repeated height changes.
- Increased the room available before the movement leash fades the view.
  Horizontal fade now starts at 25 cm and reaches black at 35 cm, up from 18/23
  cm. Upward limits are now 5/10 cm, up from 3/8 cm. Wall-contact protection is
  unchanged.

## Review fixes and diagnostics

- Invalidated the stored ship-preview pose when native on-foot VR is disabled
  or a newer camera evaluation begins.
- Improved eye dumps with menu picking geometry, authored menu surfaces, camera
  poses and bracer observations. Captures distinguish unavailable or changed
  data from a successful observation.
- Corrected diagnostic mesh reuse when the game rebuilds a picking mesh without
  changing its triangle count. Captures also reject mode values that change
  during the read.
- Pull requests now validate their merged result even when their release branch
  has already had a push build. Notes-only pull requests run the documentation
  checks; code changes run the full Windows build.
- Standardized text checkouts to LF, keeping Windows batch files at CRLF, and
  made camera-census tests independent of machine speed.

## Remaining limitations

The mission-completion menu still uses the game's curved panel and can clip
through the terminal. A flat panel fitted to the terminal is not included in
RC2. Rendering, cursor position and picking need to stay aligned for both mouse
and keyboard/controller navigation.

The on-foot mouse cursor remains hidden by default because its visible position
can drift from the click position in VR. Mouse clicks still work. The advanced
`fix.native_on_foot_panel_mouse = on` setting shows it again without resolving
that alignment problem.

## Thanks

Thanks to @dmnemec for the CI coverage, checkout consistency, review standards
and documentation improvements in PR #103.

Thanks as well to everyone who opened an issue or sent logs for this release.
Those reports are how the fixes above were found and checked.
