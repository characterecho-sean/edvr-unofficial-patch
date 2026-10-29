# Desktop window

## Status

- **State (2026-10-28):** rounds 1 to 4 FLOWN OK on Steam (`edvr_gfx_20261028_161013.log`,
  `_164251.log`, `_170329.log`; round 4, the cursor hidden over the window while it has
  focus, confirmed by Sean). ON MAIN; tester zip
  `dist\edvr-desktop-window-<commit>-dlls.zip` in this worktree. The cost against `game`
  is not measured (fps sits at the 90 Hz cap and the log carries no game-side busy
  figure); EDVR's own Present-hook work is 0.01-0.07 ms. One live key, `visual.desktop_window` (`game` |
  `left_eye` | `right_eye` | `both_eyes`, default `game`), on the Visual page of the
  in-headset menu. VR only. No temporary keys.
- **What it does:** while Elite is in VR with 3D on, EDVR makes Elite's desktop
  window a framed, sizable, movable window of the eye image's shape, fills it with
  EDVR's finished eye image, skips Elite's own drawing into it, and (round 4) hides
  the cursor over the picture. At 3D off it restores the window.
- **Round 3 results (the mouse lock):** the four cursor hooks all patched. **H1
  confirmed:** the game confines the cursor with `ClipCursor` to the client rect
  (1357x1358 at 904,45); releasing it worked and the cursor left the window (round 5
  removes that release).
  **H3 ruled out:** the game's DirectInput mouse is `0x0000000A` (nonexclusive,
  background). **H2 ruled out:** `SetCursorPos` was 0 in every window, so there is no
  re-centring (suppressing it was never needed). The game calls `ShowCursor` about 50 to
  200 times a second while focused, its last a hide that returned display count 0, so
  the cursor stays visible; none while unfocused. The game's window belongs to the
  render thread: the window-mode request ran on thread 23680, the same thread as Present.
  The 13 fps stretch from 17:04:30 to 17:05:50 was the headset asleep, not a finding.
- **Round 4, the hidden cursor** (design below): hidden when the game's window is the
  foreground window and EDVR's own `GetCursorPos` is inside the client area; visible over
  the title bar or frame, over another window, or when not foreground. Runs only when
  the window's thread is the Present thread (else `the cursor is not hidden -- the window
  belongs to thread X, Present runs on Y`, once).
- **What round 4's flight was to show** (Sean: it works): the cursor gone while over the picture with
  focus, back over the title bar and edges and when another window has focus; the cursor
  line's tail `hidden on N of M Presents, offset now K, pushed down P` (N near M while
  the cursor sits over the picture, K a small negative number, P in the hundreds a
  second only if the game's `ShowCursor(TRUE)` calls keep coming); no flash of the cursor
  on the game's own shows; the cursor back (offset 0) after 3D off; mouse flight
  unchanged (the game still sees the counts it would have).
- **Round 5, the mouse lock goes back** (BUILT; ON MAIN once merged). Sean: the game's
  mouse lock (`ClipCursor` to the client rect) is needed for on-foot VR, so round 5 removes
  the release; the cursor stays confined while the game holds it, so moving or resizing
  the window means switching away from the game first. Elite's `ClipCursor` is now
  untouched in every state: no hook on it, no call, no clip counts in the cursor line
  (`clip released ..., clip passed N, ` is gone). Kept: the `ShowCursor` hook and the
  round 4 hide, the `SetCursorPos` and `GetCursorPos` counts, the DirectInput
  cooperative-level line. The install line now lists the three hooks and says only
  `ShowCursor`'s count is changed, while the cursor is hidden.
- **Ruled out:** a stale binding shadow taking scene draws for buffer draws (the
  context named the buffer on every sampled skipped draw); H2 and H3 above.
- Older flights, the skipped-draw census and the round 3 hypotheses are under "Flight
  history" below the design.

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

- **The cursor (rounds 3 to 5)** (`desktop_window_cursor.cpp`). The executable's import
  table is patched once at startup in a VR-scope install (never a flat-only one) for
  `SetCursorPos`, `GetCursorPos` and `ShowCursor`; the install line says which
  patched. They pass through, on any thread (atomic state, no window call in them);
  only `ShowCursor`'s count is changed, and only while the cursor is hidden (below).
  **Elite's `ClipCursor` is never touched** (round 5; round 3 had released it): the
  game's lock to the client rect is needed for on-foot VR. `SetCursorPos` and
  `GetCursorPos` are only counted while an activation is committed: `setpos` and how
  many were within 2 px of the client's centre (published by the render thread each
  Present), `getpos`; `ShowCursor` is counted too (the last show/hide with the display
  count the game was given). The 10 s `cursor` line follows the census line. The
  DirectInput door (`input_gate.cpp`) sees the game's mouse at `CreateDevice` and
  observes its `SetCooperativeLevel` (slot 13) forward-first; the flags are logged once
  (`exclusive|nonexclusive, foreground|background`).

- **The hidden cursor (round 4)** (`desktop_window_cursor.cpp`, `desktopWindowCursorHide`
  at each owned Present while active). Hidden = `desktopWindowHideCursor(active,
  foreground, inside)`: `GetForegroundWindow() == hwnd` and EDVR's own `GetCursorPos`
  (its import table is not patched) inside `GetClientRect` + `ClientToScreen`. The
  show count belongs to the thread that owns the window, so only when
  `GetWindowThreadProcessId(hwnd) == GetCurrentThreadId()` in the Present hook; else
  said once and nothing done. `DesktopWindowShowCount` keeps an offset, EDVR's net
  `ShowCursor(FALSE)` calls (0 or negative, never above 0), and the last count any call
  on that thread returned. Wanted hidden and the count not known below 0: the real
  `ShowCursor(FALSE)` until it returns below 0 (offset down by one each time, at most 16).
  Not wanted: the real `ShowCursor(TRUE)` until the offset is 0. The `ShowCursor` hook,
  on that thread only and only while hiding is wanted, pushes the count below 0 after
  each game call (so a game "show" never flashes the cursor between Presents), and
  returns the real count minus the offset in force, the value the game would have seen
  without EDVR, so its own bookkeeping is undisturbed. The offset is given back when
  hiding stops being wanted and in `leave()`. The cursor line ends `hidden on N of M
  Presents, offset now K, pushed down P`.

## Flight history

- **Round 1:** `left_eye` 4032x4034; the popup 0,0 3840x2160 became 840,0 2159x2160;
  Elite resized its own swap chain one Present later; no fights; restored at 3D off.
- **Round 2:** style `0x94020000 -> 0x14CC0000` applied; first client 2031x2032; the user
  resized it to 1534x1394 and EDVR snapped it to 1393x1394, keeping the height; Elite
  resized its own chain each time, one Present later. Rendering kept going through the
  drag: about 88 Presents a second, one 59 ms frame just after. EDVR's own CPU in the
  Present hook was 0.01-0.07 ms (`desktop_window` in the frame breakdowns).
- **The skipped draws, explained:** nearly all are one pair, VS `529D6238D813A0A4` / PS
  `ED6D0B2A85BED8DD`, plain Draw, no depth view; the context check agreed on every
  sampled draw (for example 9355 of 9355). Elite really draws them into its desktop
  window. Their number varied with time, not monotonically: about 360 a Present at
  first, a peak near 2,450, then about 320. The other two pairs: Elite's mirror copy
  `01C3B84C82172B56/DED8796049C7BB4A` (2 a Present) and `1D65FB796B89CE23/9F366C4EBECFF1CE`
  (1 a Present).
- **Round 3 hypotheses** (told apart by one flight): H1 `ClipCursor` with a rectangle
  confines the cursor; H2 `SetCursorPos` re-centres it every frame (the mouse-look; if
  Elite also read `GetCursorPos` every frame, suppressing it would have broken mouse
  flight, so it was only counted); H3 a DirectInput mouse in exclusive mode. Elite's exe
  imports user32 `ClipCursor`, `SetCursorPos`, `GetCursorPos`, `ShowCursor`, `SetCursor`,
  `GetCursorInfo`, `ClientToScreen`, `ScreenToClient` and `DirectInput8Create`.

## Decisions the briefs left open

- `ASPECT_RATIO_STRETCH` fits the picture into the buffer's own shape.
- A stale frame (no new handoff since the last Present, inside 500 ms) redraws
  the held image and is counted.
- A window larger than its monitor's work area is corrected like an off-shape
  client (the size is shrunk to fit); a position alone, partly off screen, is not.
- The census also asks the context for render target 0 on one skipped draw in 32.
- `SetCursorPos` is never suppressed (round 3 ruled out H2: it is never called).
- The DirectInput mouse is observed only in a VR-scope install, on the first table.
