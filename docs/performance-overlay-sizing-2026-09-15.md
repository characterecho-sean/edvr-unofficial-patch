# Floating performance monitor sizing

## Status

- **State (2026-09-15):** Text-fit background reviewed; full build and all
  gates passed. Installed and verified in Steam and Frontier, settings
  preserved.
- **Open:** Confirm equal visible side padding in the headset.
- **Ruled out:** Fixed panel width alone does not preserve apparent size;
  independent raster/font clamps change their ratio ("Evidence and scope").
- **Next flight:** Check the single-row GPU/CPU card at low/high OpenXR
  resolution, with AA off and usual DLSS, and through F8/toast transitions. Use
  the combined check in [the timing
  guide](native-render-benchmark-2026-09-15.md).

## Evidence and scope

The floating monitor uses `buildToastContent` with a 12-degree panel width and
`menu.text_degrees * 0.85` text. The shared `sizeContent` function derives its
bitmap and font from eye pixels per degree, then clamps the bitmap width and
font independently. At 12 pixels per degree the default overlay becomes 480
pixels wide with an 11-pixel cap; at 45 pixels per degree it becomes 540 pixels
wide with a 42-pixel cap. The geometry retains its 12-degree width, so the
text-to-panel ratio and panel height change with resolution. This is a
source-level reproduction of the sizing defect, independent of any flight log.

The full native readout is formatted as `90 fps   gpu 13.3 ms   cpu 1.4 ms`. It
shares a one-line toast rectangle, uses the row font, and is drawn with
`DT_END_ELLIPSIS`. The layout never measures that text to ensure all three
fields fit. The fix must preserve the information, reserve space for values and
missing samples, and avoid changing size as ordinary readings fluctuate.

This document concerns the floating monitor's layout and apparent size. The
concurrent [application timing work](native-render-benchmark-2026-09-15.md)
defines the GPU/CPU values; layout tests do not qualify their measurement
boundaries. The full F8 settings pages and ordinary notification toasts retain
their existing layout. No configuration key is removed or renamed.

## Implementation

The monitor uses a fixed reference font and GDI-measured card width. FPS, GPU,
CPU, and any existing dropped-frame count occupy one measured row. The
reference font is smaller so the complete readout fits without clipping. The
complete current string determines the width, with 33 reference pixels of
padding on each side. The row accommodates the font's measured line height,
including descenders. An oversized field is rejected rather than truncated.

The Pimax follow-up requested one line after testing the two-row version. The
overlay now selects a 48-pixel Row face instead of the ordinary 60-pixel face
at its fixed 42-pixel reference cap. Both GDI measurement and rasterisation use
that selected face, making the text 20% smaller without tying its apparent size
to eye resolution. Full menus and notification toasts keep their font.

A further Pimax report identified excessive empty space on the right. The
single-line builder still reserved its widest possible legacy readout,
including a dropped-frame suffix even when none was displayed. Removing that
reservation makes the background fit the current text. The width can change
when the displayed text becomes shorter or longer; font size and row height
stay fixed. The compositor already uses the uploaded raster's own angular
width, so each text/background pair stays synchronized.

`menu.text_degrees` scales the angular mapping of this reference raster;
changing the eye texture size has no effect on its layout. A text-size edit
refreshes the content even when the numeric readings are unchanged. Angular
width travels with the bitmap through the worker and texture upload. The
compositor uses the uploaded bitmap's width, preventing a one-frame mismatch
when new content arrives after geometry was calculated. It suppresses an
overlay draw while the texture still belongs to the prior settings page or
toast.

## Desktop checks

The single-line menu fixture passed its GDI width and line-height checks at
`menu.text_degrees` values 0.6, 1.1 and 3.0. Cases include valid native values,
unavailable samples, startup, large values, legacy diagnostic labels and
dropped counts on one line. The fit-to-text revision checks equal side padding
for each current string while preserving font size and row height.

The earlier single-line raster was 1241x158 at all three text settings. Its
card width was 14.983 degrees at 0.6, 27.110 degrees at 1.1 and 66.660 degrees
at 3.0. The single-line maximum included the longest legacy labels and dropped
count; that fixed width explains the extra right space and is now removed.

The text-fit fixture's standard `90 fps   gpu 13.3 ms   cpu 1.4 ms` line is 690
reference pixels wide, making its card 756x158 with 33 pixels on each side. Its
angular width is 9.160, 16.711 and 43.664 degrees at text settings 0.6, 1.1 and
3.0 respectively. The existing menu test passed every padding case, and the
native WARP overlay fixture passed all 77 checks.

The native menu fixture passed 77 checks, including real WARP composition at
256x256, 512x512, 1024x512 and 512x1024 in asymmetric left/right frusta with a
flipped right-eye input. Normalized panel bounds agree within two baseline
pixels. A deliberately incorrect model width verifies that the compositor uses
the live raster's geometry, and a pending overlay cannot display the previous
settings raster. The targeted logs are `build/performance-overlay/tests-2.log`.
The combined absolute-path full build passed every gate; its log and qualified
source/binary hashes are in `build/frontier-lod-callers-20260915/attempt-15/`.
This build includes main's intervening compositor-default correction and is
stamped `v0.16.2-142-g11722af-dirty`. Both test installs were verified against
these binaries; visual confirmation of the side padding remains pending.

## Qualification plan

Exercise the production content builder against the raster's actual GDI font
measurements. Cover native readings, missing values, startup, slow frames, and
the retained legacy diagnostic labels and dropped-frame suffix. Check the
largest and smallest supported text settings, complete strings, row height,
bitmap limits, equal side padding, and stable text scale as the content
changes.

Render the monitor through the native menu provider into both eyes at several
source resolutions, with asymmetric frusta. Compare its bounds in normalized
eye coordinates, allowing only pixel rounding. This catches a correct-looking
layout helper connected to an incorrect composition path. Readbacks belong only
to the desktop test; production rendering must gain no synchronization.

Run the absolute-path full `build.bat`, including the existing menu and
native-menu fixtures and every build gate. In the next combined headset check,
compare the floating readout at low and high OpenXR render resolutions and
different AA modes, verify every displayed metric is visible, change the
text-size setting, and reopen/close F8 and a notification toast to check
transitions. Headset readability remains a manual qualification.
