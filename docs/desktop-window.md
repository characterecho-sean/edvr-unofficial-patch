# Desktop window

## Status

- **State (2026-10-28):** rounds 1 to 8 ON MAIN. **Round 8 FLOWN OK on the Pimax**
  (`edvr_gfx_20261028_215040.log`): the rule switched live 22 times, `hidden-area mesh served
  no, as the game, no mesh`, so native equals center there, as expected. OPEN: native's mesh
  path (1.32x at u 0.515) is flown on no headset; a tester with a mesh headset compares it with
  the game's own mirror. Rounds 1 to 7 FLOWN OK. **2026-10-29 (F173, Sean's word):** default
  `left_eye`, page renamed **Mirror window**. Five live keys in `[visual]`, on VR's **Mirror
  window** page (after Visual): `visual.desktop_window` (`game` | `left_eye` | `right_eye` |
  `both_eyes`, default `left_eye`), `desktop_window_aspect` (`full` | `1:1` | `4:3` | `16:10` |
  `16:9` | `21:9`, default `full`), `desktop_window_crop` (`native` | `center`, default
  `native`), `desktop_window_vertical_shift` (-100 to 100, default 0), `desktop_window_style`
  (`window` | `borderless`, default `window`). VR only. No temporary keys.
- **What it does:** while Elite is in VR with 3D on, EDVR makes Elite's desktop window a
  framed, movable, resizable window (or, in `borderless`, the whole monitor) showing EDVR's
  finished eye image, skips Elite's own drawing into it and hides the cursor over it while it
  has focus. The game's mouse lock is untouched. At 3D off the window is restored.
- **Round 6, shape and style:** a shape but `full` crops each eye (both eyes: half the width
  each) to the largest rectangle of that shape in its bounds sub-rect, kept inside the image;
  the crop rule (round 8) centres it. `window` snaps to the shape; `borderless` is the whole
  `rcMonitor` (popup style `0x94000000`), the picture fitted with black bars. A live change of
  style or shape is not a fight. Back to `window`: the last settled rect if it fits.
- **Round 7, the vertical shift:** `desktop_window_vertical_shift` moves the crop up (+) or
  down (-): from the default position, a fraction of the way to the highest (top edge at the
  picture's top) or lowest position; 0 changes nothing, +-100 reach the edges; no room, no
  move. "Up" is the side the window shows at its top (`bounds[1]`). Both eyes alike; always
  vertical, under either rule. Logs `vertical shift N` (first read, each change, `crop` line).
- **Round 8, the crop rule** (`desktop_window_crop`; menu "Same as the game's mirror" |
  "Centre"): how a chosen shape is cut; `full` is never cropped or zoomed. `center`: the shape
  around the middle of the eye's bounds sub-rect, no zoom. `native` is what Elite's own mirror
  does, read from the game (build 334925, analysis 82dc637,
  `analysis\decomp\verify_20261028_vr_mirror_crop.txt`; `Stereoscopic_FrameCombineNode::Render`,
  RVA 0x28A4200, shows the LEFT eye only and reads no field of view): no hidden-area mesh
  served is mode 7, the same as `center`; a mesh is mode 9, width and height divided by 1.32
  (RVA 0x5E86000) and centred at u 0.515, v 0.5 (pan -0.03, RVA 0x5E86004). The game never
  shows the right eye; EDVR mirrors it (u 0.485, the masks being mirror images): its own choice.
- **Round 8, the mesh flag.** The projection-centre crop (round 6) and the field of view in
  the handoff (export version 2) are retired: ruled out as the default, because the game's
  mirror crops around the texture centre and Sean chose to match it. Export **version 3**:
  `noteEye` carries whether the runtime has served the game a hidden-area mesh; the client asks
  3 then 1 (1: no flag, no mesh), 2 is refused. The game's test (`0x8D2BD0` -> `0x4E45C0` ->
  `0x4E45A0`, note R6): `(obj+0x28 != 0) || (obj+0x30 != 0)`, the counts `0x4E4910` stores
  from `GetHiddenAreaMesh(eye)` only when vertex data comes back: either eye. `OpenVRSystem`
  reports every answer with data (`noteHiddenMeshServed`, not the rate-limited trace); the host
  keeps that eye's bit, as Elite keeps its counts. No query answered: no mesh.
- **Round 8 logs:** `crop rule R` (first read), `(was Q)` (each change); per activation and eye
  `crop S, left eye, rule native: hidden-area mesh served yes, as the game, mesh: 1.32x at u
  0.515, vertical shift N; crop bounds ...` (`no, as the game, no mesh`; `center: ...`).
- **First flight of round 8 must show:** the crop row between shape and shift; with no mesh
  (the Pimax) `native` equal to `center`; with one, 1.32x at u 0.515 / 0.485; a live switch.
  `census_cb_watch = 01C3B84C82172B56`, `census_cb_slot = 2` shows Elite's own numbers.
- **Unsettled:** for a shape narrower than the eye (the both-eyes halves of 1:1, 4:3) Elite
  letterboxes the whole eye; EDVR cuts a tall rectangle, as `center` does. Elite reads the mesh
  once; EDVR follows a later answer at once.
- **Ruled out:** a stale binding shadow taking scene draws for buffer draws; H2 and H3 of
  round 3 (see "Flight history"); Elite's `ClipCursor` is never touched (round 5), so moving
  the window means switching away from the game first; centring on the projection centre
  (round 8).
- Flights 1 to 4, the draw census and rounds 3 and 4: "Flight history" below the design.

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

- **The picture and the style (round 6)** (`desktop_window.cpp`, `desktop_window_math.h`).
  `desktopWindowShapes` turns the eyes' pixel sizes into the sizes the layout uses (`full`: as
  they are; else height-1 halves or wholes of the chosen ratio, so both halves are exactly
  equal); `desktopWindowCropPlan` (round 8: the rule, the mesh flag, the eye) gives each eye's
  centre and zoom and `desktopWindowCrop` its crop bounds,
  which the draw hands to the existing shader as its bounds (so the filter's quarter-pixel taps
  and the half-texel clamp work on the crop). A flipped sub-rect stays flipped. The vertical
  shift (round 7) is `desktopWindowCrop`'s last parameter: after the default top is found and
  clamped, `top -= top * shift / 100` (shift > 0) or `top += (h - ch - top) * -shift / 100`
  (shift < 0), clamped again, in the crop's own t-space (t = 0 is the draw's top), so both eyes
  and a flipped sub-rect take the same rule. The zoom (round 8) divides the crop's width and
  height before it is centred; `full` (target 0) returns before it. The runtime asks for version 3
  of `edvrAcquireDesktopWindow` (`noteEye` carries the mesh flag) and falls back to 1; the graphics
  half answers either, and refuses 2. `desktopWindowStyleFor(mode, gameStyle)` picks the framed style or
  `desktopWindowBorderlessStyle`; a setting change recomputes `S.ourStyle`, clears
  `styleSeenOurs`, drops a request in flight and asks at once (`activeStyle`/`activeAspect`
  against the settings). Borderless asks for `desktopWindowBorderlessPlan(rcMonitor)` and
  keeps it there; it never updates the settled rect. The cursor hide, the restore and the
  mouse lock are as before. The menu page is `menu desktop_window` (VR only: no flat table
  lists it), built right after Visual from the Fix-tier rows of that page, in file order.

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

- **Round 3 results (the mouse lock):** the four cursor hooks all patched. **H1 confirmed:**
  the game confines the cursor with `ClipCursor` to the client rect (1357x1358 at 904,45);
  releasing it worked and the cursor left the window (round 5 removed that release). **H3 ruled
  out:** the game's DirectInput mouse is `0x0000000A` (nonexclusive, background). **H2 ruled
  out:** `SetCursorPos` was 0 in every window. The game calls `ShowCursor` about 50 to 200 times
  a second while focused, its last a hide that returned display count 0, so the cursor stays
  visible; none while unfocused. The game's window belongs to the render thread: the window-mode
  request ran on thread 23680, the same thread as Present. The 13 fps stretch from 17:04:30 to
  17:05:50 was the headset asleep, not a finding.
- **Round 4, the hidden cursor:** hidden when the game's window is the foreground window and
  EDVR's own `GetCursorPos` is inside the client area; visible over the title bar or frame, over
  another window, or when not foreground. Runs only when the window's thread is the Present
  thread (else `the cursor is not hidden -- the window belongs to thread X, Present runs on Y`,
  once). Sean: it works. The cursor line's tail is `hidden on N of M Presents, offset now K,
  pushed down P`.
- **Round 5:** the release of the game's lock was removed (see Status); the cursor line lost its
  clip counts, the install line lists three hooks.
- **Round 6 (`edvr_gfx_20261028_192619.log`):** the Desktop window page after Visual with three
  rows; `aspect` and `style` lines at the start and on each change; 16:9 cropped around each
  eye's centre (right eye u 0.4108, left u 0.5892, v 0.5000) and two 8:9 halves with
  `both_eyes`; `borderless` filling the monitor and back to `window`, live both ways; the mouse
  lock and the hidden cursor working in both styles; restored at 3D off. `--health` OK. (The
  crop centre was the projection centre then; round 8 retired that for Elite's own rule.)
- **Round 7:** the vertical shift flown OK (Sean: looks good).

## Decisions the briefs left open

- `ASPECT_RATIO_STRETCH` fits the picture into the buffer's own shape.
- A stale frame (no new handoff since the last Present, inside 500 ms) redraws
  the held image and is counted.
- A window larger than its monitor's work area is corrected like an off-shape
  client (the size is shrunk to fit); a position alone, partly off screen, is not.
- The census also asks the context for render target 0 on one skipped draw in 32.
- `SetCursorPos` is never suppressed (round 3 ruled out H2: it is never called).
- The DirectInput mouse is observed only in a VR-scope install, on the first table.
- `desktop_window_aspect`'s ':' values were kept (they pass the schema generator and the ini
  parser); a `16x9` spelling would have been the fallback.
- A shape or style change while the game has fought the window too often (`gaveUp`) is not acted
  on until the next session.
- In `borderless` the cursor hide uses the whole monitor as the client area.
