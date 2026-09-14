# OpenXR resolution control

The Performance page gains an OpenXR resolution percentage. The default is
100%, using the active runtime's recommended per-eye width and height. The
percentage scales each dimension: 50% means half the width and half the height,
or one quarter of the pixels. The supported selection range is 25–200%. Windows
continues to select the runtime.

The setting requires restarting Elite. Startup chooses the eye-buffer
recommendation and creates the OpenXR swapchains at the selected size before
publishing geometry to the game and the temporal provider. A running session
keeps its active size; the existing menu restart indicator tracks saved
changes. This avoids partially resizing the renderer or presenting a saved
value as already active.

The menu also shows the selected target in pixels per eye using the current
runtime's recommendations and size limits. Active and pending dimensions are
distinguished. If the runtime limits the selection, the displayed target
reflects that limit. A runtime change at the next launch can change the
recommendation, so this is a preview for the currently connected runtime.

The row places the percentage beside the compact label (`OpenXR res. 75%`) and
the pixel dimensions in the value column. If the eyes differ, the row labels
the left eye with `L`; highlighting it shows both eyes, along with the active
and after-restart sizes. The tooltip also identifies any runtime cap. Editing
retains the menu's existing numeric-factor input convention: type `0.75` for
75%, or use Left/Right to step.

One shared calculation controls startup sizing and the menu preview. A common
scale is capped against both eyes' runtime limits and the D3D11 texture limit,
preserving aspect ratio apart from integer rounding. Resolution state belongs
to the native session and is invalidated when it closes. Without a current
native session the menu cannot invent a pixel preview.

This controls the OpenXR target. Elite's HMD image-quality multiplier and DLSS
input scaling still apply separately. It does not change Windows runtime
selection or the vendor application's global rendering settings.

## Retest

1. At 100%, check the per-eye dimensions shown on the Performance page and
   confirm normal rendering and tracking.
2. Select a lower percentage. Check the pixel preview and restart indication;
   the current session's active dimensions must remain unchanged.
3. Exit and relaunch Elite manually. Check that the selected dimensions are now
   active and that TAA/DLSS, F8 and head tracking behave normally.
4. Restore 100%, restart and verify the original runtime recommendation is used
   again. A capped value should show the resulting constrained target.

This retest can be combined with the corrected [Air Link startup
route](openxr-oculus-selection-2026-09-14.md) and the outstanding [supported
feature checklist](openxr-feature-parity-2026-09-14.md). The installed pair is
recorded below.

## Desktop checks

The first full run caught a fixture assertion that bounded `1000 / 781` below
1.28; its expected bound was corrected to include the actual 1.2804 factor. A
subsequent run stopped because the existing census bridge's successful
short-lived child left both logs empty. The unchanged seven-case census test
passed immediately on rerun; the empty-log artifact is retained as
`build/openxr-airlink-resolution-empty-log-build.log`. No GPU timing assertion
was removed or weakened.

The production settings provider passes 41 checks for configuration, versioned
queries, invalid inputs, coherent sizing, saved-versus-active values and
generation ownership. The host's inert startup tests exercise 100%, 50%,
asymmetric eyes, a common runtime limit and capture-once behavior without
opening a runtime or device.

GDI measurement caught clipping in the initial combined percentage/dimensions
value. With the final split layout at the default sampled card size (818
pixels, 30-pixel cap), `OpenXR res. 200%` measures 344 pixels in the 466-pixel
label area; `L16384x16384` measures 269 pixels in the 304-pixel value area. The
longest distinct-eye hint wraps to 92 pixels in 94 pixels of usable height.
Both the measured strings and the menu use Segoe UI at the same sizes. Headset
readability remains part of the manual retest.

## Installed qualification

The final absolute-path `build.bat` run passed with all 537 source hashes
unchanged. The contract accounts for 255 keys; the native host passes 77 checks
and each actual graphics DLL variant passes 18 startup checks. Actual TAA and
DLSS output tests pass 169 checks each, with 18 treated eyes per mode;
sharpening on/off passes 58/22. These desktop checks do not establish headset
image quality.

Frontier is installed and verified with `v0.16.2-94-g663b49d-dirty`:

- Native runtime SHA-256:
  `77ce22d8bd544fb3e96e3396ace8a76cf5f3dedc459af13ea5ac795e4e9be804`.
- Native graphics SHA-256:
  `d377ede3012560c0884314b7f51e12c86b58d6e01a925f619385c5592a3a8d70`.
- Startup configuration SHA-256:
  `3da530c6e9c7e82b99a9f10caf0f6ea8b467719b36837aa4e2cbb37bd924f7b2`.

The installer preview, full build log, source/artifact hashes, GPU results and
verification are archived in
`build/openxr-airlink-resolution-20260914/qualification.json`. Windows runtime
discovery remains `system`; the user's `edvr.ini` and preserved original OpenVR
DLL hashes are unchanged. No game or launcher was started. Air Link routing and
in-headset scale changes remain pending the manual retest.
