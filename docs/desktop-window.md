# Desktop window

## Status

- **State (2026-10-28):** BUILT, NOT FLOWN. Branch
  `claude/elite-desktop-mirror-window-8af2d9`, installed on Steam for the
  first flight. One live key, `visual.desktop_window` (`game` | `left_eye` |
  `right_eye` | `both_eyes`, default `game`), on the Visual page of the
  in-headset menu. VR profile only.
- **What it does:** while Elite is in VR with 3D on, EDVR reshapes Elite's
  own desktop window to the shape of the eye image (as large as fits on the
  monitor the window is on) and fills it with EDVR's finished eye image:
  after DLSS/DLAA/FSR, sharpening, the UI layer and EDVR's menu. Elite's
  own drawing into that window is skipped. When no eye image arrives (3D
  off) EDVR stops, puts the window back where Elite had it, and Elite's
  window takes over again.
- **What the first flight must show** (log lines start `desktop window:`;
  `python tools\edvr_log.py --target steam --expect-build HEAD --health`
  says OK or NEVER APPLIED):
  1. The window reshaped and filled: a `reshape request` line, then a
     `10 s:` line with `drawn` above 0 and a client size in the eye's shape.
  2. Elite's draws skipped: `skipped_draws` on the `10 s:` line rises (it
     counts Elite's draws into the back buffer, at least one a Present
     while Elite's mirror is on), and the desktop shows no flicker
     between our picture and Elite's.
  3. Whether Elite resizes its swap chain when the window changes size: the
     `swap chain now WxH (was WxH); N Presents since the last reshape
     request` line. No line means it does not, and the picture then relies
     on the draw's fit (STRETCH: the whole buffer is stretched onto the
     client).
  4. Whether Elite fights the move: `the window was moved off its target`
     lines. More than 10 and EDVR stops reshaping for the session (the
     picture is still drawn, undistorted, with bars).
  5. Restore at 3D off: the `stopped (...)` line says `restored to ...`,
     `left alone (the game moved it ...)` or `never left the game's place`.
  6. Whether `DisplaySettings.xml` (and Settings.xml) changed after the
     flight, as Elite may write its window position and size on exit.
- **Open hypotheses:** none proven. Doubts to check on the first flight:
  a withheld frame (the transition-flash fix) redraws the held image, which
  for a pass with no stages is Elite's own eye texture, already being
  overwritten (see "Decisions"); and DPI: the frame the plan allows for is
  `AdjustWindowRectEx`'s, not the per-monitor DPI one.
- **Ruled out:** nothing yet.
- **Temporary keys:** none. The key is a user setting.
- **Next flight:** `visual.desktop_window = both_eyes`, then `left_eye`,
  with 3D on and off, windowed borderless and windowed with a caption.

## Design

- **The handoff.** The runtime (`src\openxr\native_runtime_host.h`,
  `capture()`) calls `noteEye` right after an eye is captured, with the
  texture it is about to give the headset (`treated`) and its bounds. The
  texture is on Elite's device: EDVR's own treat output, or Elite's eye
  texture when no stage ran. Its pixels stay unchanged until that eye's next
  Submit, and Elite submits left, right, then Presents, all on the render
  thread, so at Present the frame's images are ready. The call goes through
  a versioned export of the graphics half, `edvrAcquireDesktopWindow`
  (`src\common\native_desktop_window.h`), optional on both sides. The
  graphics half takes a reference and keeps the latest of each eye the
  setting needs; a displaced texture is released on the render thread.
- **Active** = the setting is not `game`, the profile is VR, and the needed
  eye(s) were handed over within the last 500 ms. Not active: nothing is
  skipped or drawn, the window is restored, the held textures are released.
- **The draw** (`src\d3d11\desktop_window.cpp`), at each owned Present,
  before the real Present and inside the census span: clear the buffer to
  black, draw the eye (or both, left on the left) aspect-fit and centred
  into the part of the buffer the client shows. Scaling STRETCH (and every
  blit-model chain): the whole buffer is stretched onto the client, so the
  picture is fitted to the client's shape and then mapped to the buffer.
  NONE: the top-left client-sized region. ASPECT_RATIO_STRETCH: fitted into
  the buffer. Filter: four bilinear taps a quarter of a destination pixel
  either side, held inside the eye's bounds. Formats pass bytes through
  (typeless source gets its UNORM view; a typed sRGB source an sRGB target
  on an 8-bit buffer); any other combination draws nothing, skips nothing
  and says why once. The back buffer and its view are local to the hook.
- **The skip.** At each Present the buffer's identity (a raw pointer, no
  reference) is recorded; every Elite draw whose render target 0 resolves to
  it is skipped, in the draw verdict and the three thunks that do not take
  one (`vscreen.cpp`). Off, the cost is one relaxed load per draw.
- **The window.** `hwnd` is the swap chain's `OutputWindow`. Bound = the
  monitor's work area for a captioned window, else the monitor; target = the
  largest client of the eye's shape whose `AdjustWindowRectEx` rectangle
  fits, centred. A move is requested at most once a second, from a
  thread-pool callback with `SWP_ASYNCWINDOWPOS`, never from the Present
  hook. Not reshaped in exclusive fullscreen, when maximized or when
  minimized. No game address is hooked, so it works on every build.
- **Restore.** If the window is still at EDVR's last target when the
  activation ends, it is moved back to the rect remembered before the first
  move. If the game moved it (its own 3D-off mode change), it is left alone.

## Decisions the brief left open

- `ASPECT_RATIO_STRETCH` fits the picture into the buffer's own shape.
- A stale frame (no new handoff since the last Present, inside 500 ms)
  redraws the held image and is counted.
- EDVR also stops after 20 move requests in a row that never reached their
  target (a window the system clamps), besides the 10-fight limit.
- The key is on the in-headset Visual page (`menu visual`), live.
