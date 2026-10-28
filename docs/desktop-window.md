# Desktop window

## Status

- **State (2026-10-28):** round 1 FLOWN OK on Steam (`edvr_gfx_20261028_161013.log`).
  Round 2 (an ordinary movable, resizable window; the skipped-draw census)
  BUILT, NOT FLOWN. One live key, `visual.desktop_window` (`game` | `left_eye` |
  `right_eye` | `both_eyes`, default `game`), on the Visual page of the
  in-headset menu. VR profile only.
- **What it does:** while Elite is in VR with 3D on, EDVR makes Elite's desktop
  window a framed, sizable window of the eye image's shape and fills it with
  EDVR's finished eye image (after DLSS/DLAA/FSR, sharpening, the UI layer and
  EDVR's menu). Elite's own drawing into it is skipped. With no eye image (3D off)
  EDVR stops, restores the window, and Elite's window takes over.
- **Round 1 flight:** active with `left_eye` 4032x4034; the popup window
  0,0 3840x2160 was reshaped to 840,0 2159x2160; Elite resized its own swap chain
  to 2159x2160 one Present later; no fights; restored at 3D off.
- **Unexplained:** Elite's skipped draws per Present climbed steadily, about 365,
  527, 755, 961, 1205 in successive 10 s windows (`skipped_draws` over `drawn`),
  far more than a mirror copy. The instrument is the census line after each
  `10 s:` line: `skipped by shader (10 s)`, the top six VS/PS pairs, `other`,
  `pairs`, `with depth`, the draw kinds, and `render target 0 checked on N, the
  buffer on M` (one skipped draw in 32 is asked of the context itself: M below N
  means the binding shadow's answer is stale). Hypotheses to separate: (a) the
  shadow's render target 0 is stale and scene draws are taken for buffer draws
  (checked N, buffer M with M much below N; a spread of scene shader hashes);
  (b) Elite really draws a growing number of things into the buffer, such as a
  desktop UI or a flat re-render (M equals N; a few hashes, the count of one pair
  growing); (c) a depth view bound means a scene pass (`with depth`).
- **First flight of round 2 must show:** the style `0x94020000 -> 0x14CC0000` in the
  `reshape request` line and a framed window; moving it by the title bar and
  resizing it by an edge or corner (the window follows the picture's shape a
  second after letting go, the dimension changed most kept; `moves=`/`resizes=` on
  the `10 s:` line); no correction while dragging; deactivating at 3D off with
  `restored to ... style 0x94020000 back`; a second activation starting where the
  window last was; and the census. Dragging may pause Elite's rendering if its
  message pump runs on the render thread, as in Elite's own windowed mode; the
  flight shows whether it does (frame pacing in the `10 s:` lines while dragging).
- **Ruled out:** nothing yet. **Temporary keys:** none.

## Design

- **The handoff.** `capture()` (`native_runtime_host.h`) calls `noteEye` right after
  an eye is captured, with the texture it is about to give the headset and its
  bounds, through the optional export `edvrAcquireDesktopWindow`
  (`native_desktop_window.h`). The texture is on Elite's device and unchanged
  until that eye's next Submit; Elite submits left, right, then Presents on one
  thread, so at Present the images are ready. The graphics half keeps a reference
  to the latest of each eye the setting needs; displaced ones are released on the
  render thread. **Active** = the setting is not `game`, the profile is VR, and the
  needed eye(s) were handed over within 500 ms.
- **The draw** (`desktop_window.cpp`), at each owned Present, before the real
  Present: clear to black, draw the eye (both: left on the left) aspect-fit into
  the part of the buffer the client shows (STRETCH: fitted to the client and
  stretched onto the buffer; NONE: the top-left client-sized region;
  ASPECT_RATIO_STRETCH: fitted to the buffer). Four bilinear taps a quarter
  destination pixel either side, held inside the bounds. Formats pass bytes
  through; any other combination draws nothing, skips nothing, says why once.
- **The skip** (`vscreen.cpp`). The buffer's identity (a raw pointer, no
  reference) is recorded at each Present; every Elite draw whose render target 0
  resolves to it is skipped, in the draw verdict and the three thunks that do not
  take one. Off, the cost is one relaxed load per draw. A skipped draw is also
  counted by `(VS hash, PS hash)` in a 16-pair table, by kind (draw, indexed,
  instanced, indexed instanced, indirect, auto) and by a depth view bound; no
  allocation, no lock (`desktop_window_census.h`; the skip sites are all on
  Elite's immediate context and the draw hooks and Present are one thread).
- **The window, round 2.** While active the window is framed and sizable
  whatever the game had: `style = (style & ~(POPUP | MAXIMIZEBOX | MINIMIZEBOX |
  MAXIMIZE | MINIMIZE)) | CAPTION | SYSMENU | THICKFRAME` (`0x94020000` becomes
  `0x14CC0000`); no minimize box, because a minimized window could stop Elite
  rendering. The game's style, exStyle and rect are remembered and restored. A
  request sets the style (`SetWindowLongPtrW`) and then the rectangle
  (`SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS | ...`) from a thread-pool callback,
  never from the Present hook; at most one a second, reads at most every 100 ms.
  The user owns position and size; EDVR keeps the aspect:
  1. First activation: the largest client of the eye's aspect in the work area of
     the window's monitor, centred (frame from `AdjustWindowRectExForDpi`, else
     `AdjustWindowRectEx`). Later activations: the window rect last had at a
     settled aspect, if it still lies inside a work area; else as the first.
  2. While the window's thread is in a move or size loop (`GUI_INMOVESIZE`),
     nothing; the draw keeps the picture undistorted, with bars.
  3. After it, a client off the aspect by more than 2 px on either axis (or a
     window larger than the work area) is corrected: the dimension that changed
     more from the last settled client size is kept (the width when none), the
     other follows the aspect, the top-left stays, then shrunk to the work area,
     a client height of at least 240, and slid inside the work area. The frame
     is measured from the window once our style is on it.
  4. A position change alone is never corrected; another monitor is fine.
  5. Settled (client within 2 px of the aspect, style ours): that rect is
     remembered. 6. A fight is only the style changing away from ours while active
     (Elite re-applied its mode): counted, the style re-applied, more than 10 and
     reshaping stops for the session. User moves and resizes are not fights; 20
     requests of ours in a row that did not land also stop it.
  7. Restore: if the style is still ours, the game's style and rect go back; if
     the game changed the style itself, the window is left alone. Both are logged.
  Not reshaped in exclusive fullscreen, maximized or minimized. No game address
  is hooked, so it works on every build.

## Decisions the briefs left open

- `ASPECT_RATIO_STRETCH` fits the picture into the buffer's own shape.
- A stale frame (no new handoff since the last Present, inside 500 ms) redraws
  the held image and is counted.
- A window larger than its monitor's work area is corrected like an off-shape
  client (the size is shrunk to fit); a position alone, partly off screen, is not.
- The census also asks the context for render target 0 on one skipped draw in 32.
