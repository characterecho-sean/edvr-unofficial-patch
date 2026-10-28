# Desktop window

## Status

- **State (2026-10-28):** rounds 1 and 2 FLOWN OK on Steam (`edvr_gfx_20261028_161013.log`,
  `edvr_gfx_20261028_164251.log`). Round 3 (the cursor can leave the window) BUILT,
  NOT FLOWN. One live key, `visual.desktop_window` (`game` | `left_eye` | `right_eye` |
  `both_eyes`, default `game`), on the Visual page of the in-headset menu. VR only.
- **What it does:** while Elite is in VR with 3D on, EDVR makes Elite's desktop
  window a framed, sizable window of the eye image's shape and fills it with
  EDVR's finished eye image (after DLSS/DLAA/FSR, sharpening, the UI layer and
  EDVR's menu). Elite's own drawing into it is skipped. With no eye image (3D off)
  EDVR stops, restores the window, and Elite's window takes over.
- **Round 1 flight:** `left_eye` 4032x4034; the popup 0,0 3840x2160 became
  840,0 2159x2160; Elite resized its own swap chain one Present later; no fights;
  restored at 3D off.
- **Round 2 flight:** style `0x94020000 -> 0x14CC0000` applied; first client
  2031x2032; the user resized it to 1534x1394 and EDVR snapped it to 1393x1394,
  keeping the height; Elite resized its own chain each time, one Present later.
  Rendering kept going through the drag: about 88 Presents a second across it, one
  59 ms frame just after. EDVR's own CPU in the Present hook was 0.01-0.07 ms
  (`desktop_window` in the frame breakdowns).
- **The skipped draws, explained:** the census says nearly all are one pair, VS
  `529D6238D813A0A4` / PS `ED6D0B2A85BED8DD`, plain Draw, no depth view; the context
  check agreed on every sampled draw (for example 9355 of 9355). Elite really draws
  them into its desktop window. Their number varied with time, not monotonically:
  about 360 a Present at first, a peak near 2,450, then down to about 320. The other
  two pairs: Elite's mirror copy `01C3B84C82172B56/DED8796049C7BB4A` (2 a Present) and
  `1D65FB796B89CE23/9F366C4EBECFF1CE` (1 a Present).
- **Ruled out:** a stale binding shadow taking scene draws for buffer draws, because
  the context itself named the buffer on every sampled skipped draw.
- **Round 3, the mouse lock.** The cursor cannot leave Elite's window. Elite's exe
  imports user32 `ClipCursor`, `SetCursorPos`, `GetCursorPos`, `ShowCursor`,
  `SetCursor`, `GetCursorInfo`, `ClientToScreen`, `ScreenToClient` and
  `DirectInput8Create`. Which makes the lock is NOT known. Hypotheses, to be told
  apart by one flight: **H1** `ClipCursor` with a rectangle confines it (the line's
  `clip released` is above 0 and the cursor is free); **H2** `SetCursorPos` re-centres
  it every frame, the mouse-look lock (`setpos` near the Present rate with most `at
  the client centre`); if Elite also reads `GetCursorPos` every frame it steers by the
  cursor's movement, and suppressing `SetCursorPos` would break mouse flight, so it
  is counted and NOT changed; **H3** a DirectInput mouse in exclusive mode, neither
  user32 call involved (the `DirectInput mouse cooperative level` line says
  exclusive or not).
- **First flight of round 3 must show:** the install line (which of the four hooks
  patched), the cursor line after each census line, `clip released` and its last rect,
  the cooperative level once, and whether the cursor can leave the window with the
  window active, and is confined again after 3D off (Elite sets its own clip again).
  If the cursor is still held with `clip released` 0 and `setpos` high, it is H2 or H3.
- **Temporary keys:** none.

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

- **The cursor (round 3)** (`desktop_window_cursor.cpp`). The executable's import
  table is patched once at startup in a VR-scope install (never a flat-only one) for
  `ClipCursor`, `SetCursorPos`, `GetCursorPos` and `ShowCursor`; the install line says
  which patched. They pass through, on any thread (atomic state, no window call in
  them), except `ClipCursor` with a rectangle while an activation is committed: the
  real `ClipCursor(nullptr)` is called, TRUE is returned and it is counted. `ClipCursor(nullptr)`
  passes. At each activation the real `ClipCursor(nullptr)` is called once from the
  render thread. Not active, nothing is touched. `SetCursorPos`, `GetCursorPos` and
  `ShowCursor` are only counted: `setpos` and how many were within 2 px of the client's
  centre (published by the render thread each Present), `getpos`, and the last
  show/hide with the display count. The 10 s `cursor` line follows the census line.
  The DirectInput door (`input_gate.cpp`) sees the game's mouse at `CreateDevice` and
  observes its `SetCooperativeLevel` (slot 13) forward-first; the flags are logged once
  (`exclusive|nonexclusive, foreground|background`).

## Decisions the briefs left open

- `ASPECT_RATIO_STRETCH` fits the picture into the buffer's own shape.
- A stale frame (no new handoff since the last Present, inside 500 ms) redraws
  the held image and is counted.
- A window larger than its monitor's work area is corrected like an off-shape
  client (the size is shrunk to fit); a position alone, partly off screen, is not.
- The census also asks the context for render target 0 on one skipped draw in 32.
- `SetCursorPos` is never suppressed this round (H2: it could be the mouse-look).
- The DirectInput mouse is observed only in a VR-scope install, on the first table.
