# First native PiOpenXR stereo run

The standalone diagnostic from `93e7f52` completed successfully on the Pimax
headset. The user saw the multicolored triangle. SteamVR and Frontier processes
were absent before launch, and SteamVR processes were still absent after the
diagnostic exited. The test selected PiOpenXR only in its child process
environment; no installed game files or registry settings changed.

The executable SHA-256 in the run receipt matches the locally retained
full-build validation record. Loader and runtime-manifest hashes also match the
earlier capability inventory. Exact receipts, executable hashes, paths and raw
tracking poses remain local under `build`; this document records the relevant
result without that machine-specific data.

## Runtime result

| Measurement | Observed result |
| --- | --- |
| Runtime identity | Pimax OpenXR, reported version 0.1.0 |
| Eye swapchain dimensions | 5424 x 5356 each |
| Created device feature level | D3D 11.1, on the requested adapter |
| Selected color format | DXGI 29, RGBA8 sRGB |
| Requested running interval | 20 seconds |
| Total process duration | Approximately 20.36 seconds |
| Begun frames | 1801 |
| Successful stereo submissions | 1800 |
| Zero-layer frames | 1 |
| Valid eye-view samples | 1800 |
| Invalid eye-view samples | 0 |
| Valid HMD-location samples | 1800 |
| Normal session stop / cleanup | Both succeeded |
| Child exit / watchdog | Exit 0; watchdog did not fire |

The first reported left FOV angles are approximately `(-0.991678, 0.801335,
0.901787, -0.901787)` in left/right/up/down order; the right view mirrors the
horizontal asymmetry. Both eyes and the separately located HMD report
orientation/position valid and tracked flags in the initial sample. These are
native runtime results, distinct from the earlier game-facing SteamVR census
values. The renderer preserved the supplied native poses and FOVs in its
projection views.

This establishes real loader, adapter/device, session, spaces, native geometry,
swapchains, stereo submission and normal shutdown for this diagnostic on the
installed PiOpenXR configuration. The user confirmed seeing its triangle.
Explicit visual confirmation of each eye and stability during head movement is
still pending; API validity and successful submission alone do not establish
visual tracking quality.

## Remaining boundary

This was a standalone test scene using a diagnostic-owned D3D11 device.
Frontier remains on the tested OpenVR census build `f3c205e`. Launching
Frontier directly through OpenXR still requires the game-facing OpenVR
implementations, binding to Elite's validated graphics device, a startup
geometry snapshot and thread/lifetime ownership, stereo image capture and EDVR
feature integration.

The result does not qualify runtime loss, doff/don, recenter, repeated
initialization, the other installed runtimes, game image quality, timing
overhead, or feature parity. Frame counts from this simple scene are not a game
performance comparison. The next implementation step can use this native path
as a reference while retaining those separate acceptance gates.
