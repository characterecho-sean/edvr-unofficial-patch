# Night vision controls

Night vision pulse stability remains a standard, enabled Fixes control:
`fix.night_vision_stability = 1`. It corrects the pulse's distance under
head rotation while retaining the original normal-map outlines, surface
shading, brightness and stencil behavior. It works with AA Off, TAA and
DLSS. Its measured camera and draw contracts are documented in [the
original investigation](review-planet-performance-2026-09-11.md).

Realistic nightvision is now an optional appearance under Experimental:
`experimental.night_vision_realistic = 0` by default. It outlines depth
geometry, suppresses fine normal-map outlines, excludes cockpit/body
surfaces, and brightens the original exterior texture without a green
fill. Its brightness control is `experimental.night_vision_brightness =
8.0`, range 1..16. These settings are live and require the menu's
developer pages to be visible.

The old `fix.night_vision_stability` key now controls only pulse
stability; an existing value of 1 does not opt a user into the
experimental appearance. Saved `fix.night_vision_brightness` values
migrate to the new experimental brightness key. The earlier advanced
brightness location also remains recognized, with its former 2.0 default
treated as stale. Migrating brightness does not enable Realistic
nightvision.

Pulse stability and appearance have independent shader variants. Pulse
stability alone changes only the radial pulse-distance calculation, uses
the game's original blend and stencil, and allocates no exterior mask or
brightness buffer. The experimental path retains its existing stencil
classification and dual-source blending. Both switches off use the
original game shader. Each draw restores the state it changed even if a
setting changes before the draw ends; failure in the experimental
variant does not disable the independent pulse-only variant.

GPU regressions exercise the same radial pulse under two rotated,
asymmetric projections in both standard and experimental modes. They
also compare complete pulse-only and stock outputs with the pulse at its
floor, including patterned normal maps, terrain texture, artistic
pixelation and body stencil. Further checks cover independent live
switches, absent experimental stencil resources, shader failure
isolation and state restoration. The original geometry and brightness
tests remain in place.

Validation: the full absolute-path build and configuration contract
pass. The night-vision suite passes on both WARP and NVIDIA hardware,
and the production DLL passes the NVIDIA smoke test. Generated menu rows
place pulse stability under Fixes and both appearance controls under
Experimental; the runtime migration table maps both older brightness
locations correctly.
