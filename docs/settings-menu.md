# The in-headset settings menu: a design

*A design document, written before the code. It supersedes and extends
Feature 4 of [performance.md](performance.md) (2026-09-05), which stays as
the origin and now points here. Claims about EDVR cite the source; claims
about the game and about Windows are labelled MEASURED (established at the
desk or in a field log kept by this repo) or BELIEVED (inference, each with
a gate below that turns it into a measurement before code may depend on
it); what can only be settled with a headset on is collected under Phase 0.
Written 2026-09-07 on branch `claude/ingame-settings-menu-78317d` off main
`5e2d545`.*

*Status, 2026-09-07, later the same day: **phase A is BUILT and UNFLOWN**
on the same branch -- the three doors (`src/d3d11/input_gate.cpp` over
`src/common/iat_hook.cpp`), the model and the ini write
(`src/d3d11/menu.cpp`), the GDI raster and the compute composite
(`src/d3d11/menu_panel.cpp`, export `edvrMenuPanel`), the door's caller
(`src/openvr/menu_door.cpp`), the channel fields (`frame_flag` `_v24`),
the generated row table (`menu_schema.inc` from
`tools/gen_settings_schema.py`), the `[menu]` section and `hotkey.menu = F8`
in `edvr.ini`, and `tools/menu_test`. Two things differ from the text
below and are marked where they occur: the developer tier's restart
enforcement is a build WARNING with a `?` badge rather than an error (61
keys in `[advanced]` and `[experimental]` do not yet say when they apply,
and rewriting 61 comments was not this change's job), and `iniedit` moved
to `src/common` in namespace `edvr`. Every Phase 0 gate is still open.*

*Flown 2026-09-07, the same evening, on the Pimax Crystal Super under
SteamVR: the panel appears where you look and holds still, the keys are
private, the rows write the ini. Two changes from that flight, both below:
head-aim fought the arrow keys and is off by default now (`menu.aim =
keys`), and the frame-time line left the Status page for a **Monitor
page** with fpsVR's readout and a frame-time strip, plus an optional
head-locked readout while the menu is closed (`menu.fps_overlay`), the
toolkit's overlay.*

## The ask

Sean, 2026-09-07: an in-game menu that lets players switch settings
quickly, "similar to what's in OpenXR Toolkit", fleshed out from the
Feature 4 sketch, with three additions:

1. **A developer key in the ini** that also exposes the advanced and
   experimental options.
2. **Keyboard input that does not pass through to the game.**
3. **The panel lives in 3D space**, not head-locked the way the toolkit's is.

And the same day: **flag which settings need a restart** when they are
changed.

## The short version

Feature 4 was built on one assumption it inherited from `hotkey.h`: EDVR
watches keys and cannot take them, so the menu must be driven by head-aim
and two carefully chosen chords that the game also receives. That was a
policy, not a fact. MEASURED at the desk today: Elite reads the keyboard
through exactly three doors, all of them in this process and all patchable
without a system-wide hook (the section "The keyboard gate"). So while the
menu is open **the game sees no keyboard at all** -- every key is released
from its point of view, and every press belongs to the menu. That single
change turns the whole toolkit navigation model on: arrows, Enter, Escape,
page keys, hold-to-repeat, and later typing a value, none of it colliding
with a ship binding. Head-aim stays, as the hands-free way to pick a row.

Everything else Feature 4 decided survives and is restated briefly:
world-anchored where you look, type sized in degrees, drawn last at the
door onto the native-size outgoing frame, every row wearing its measured
price, the ini as the single source of truth. Two things are new besides
the gate: a **developer tier** (`menu.developer = on`) that adds the
Advanced, Experimental and Instruments pages and shows the ini's own key
names beside the labels, and **restart flagging** that is enforced at build
time, shown on the row, counted in the footer, and repeated in the log.

## What the toolkit got wrong, restated as the brief

Head-locked text that rode every head movement; pixel-fixed type that came
out tiny on wide-FOV headsets; three chorded function keys to walk a tree,
chosen because an OpenXR layer cannot take keys from the game either; and
no sense of what a setting *cost* beyond watching the FPS counter wobble.
This design answers each: the panel stays in the world; text is specified
in degrees; the keyboard is the menu's while it is open, so the keys are
the obvious ones; and each row shows its measured cost.

## The keyboard gate

### What Elite reads (MEASURED 2026-09-07)

`dumpbin /IMPORTS` on `EliteDangerous64.exe` (the Steam install, the build
running on 2026-09-07):

| Module | Keyboard-relevant imports |
|---|---|
| `DINPUT8.dll` | `DirectInput8Create` |
| `USER32.dll` | `GetAsyncKeyState`, `GetKeyState`, `GetKeyboardState`, `PeekMessageA`, `TranslateMessage`, `DispatchMessageA`, `ToUnicodeEx`, `MapVirtualKeyExA`, `SetWindowsHookExA` |
| `XINPUT9_1_0.dll` | `XInputGetState`, `XInputSetState` |

Absent: `GetRawInputData`, `GetRawInputBuffer`, `RegisterRawInputDevices`,
and every `GetMessage` variant. Elite does not use Raw Input for the
keyboard (a static-import fact; a dynamic `GetProcAddress` of it would be a
surprise the probe below would catch as "a key that leaked"). The bindings
path is DirectInput: `hotkey.cpp` recorded on 2026-08-16 that Elite keeps
delivering bound keys while its window is not foreground, which only
DirectInput does. BELIEVED, from the import list's shape: the user32 trio
serves text fields and UI state, and the message pump's `WM_CHAR` (made by
`TranslateMessage` from `WM_KEYDOWN`) serves typing. `SetWindowsHookExA` is
BELIEVED to be the usual game-side low-level hook that disables the Windows
key; if it is instead a keyboard hook the game reads through, gate G3's
modal test is where that shows.

### Three doors, one flag

One atomic flag, `g_keyboardPrivate`, set by the menu when it is visible
and cleared when it is not. Three thin hooks consult it. Each is installed
once, on the menu's first use, and each degrades to pass-through on any
doubt.

**Door 1 -- DirectInput's keyboard device (the bindings path).**
`IDirectInputDevice8::GetDeviceState` (slot 9) and `GetDeviceData` (slot
10). EDVR reaches the game's device object without ever seeing it: it
calls `DirectInput8Create` itself, creates its own `GUID_SysKeyboard`
device, and patches that device's vtable **in place**. BELIEVED: every
device object dinput8.dll makes dispatches through the same static table,
so the game's keyboard device arrives at the same thunks (this is the
standard way overlays reach a game's DirectInput devices, and the
`vtable_hook.h` comment already states the consequence: "patching a vtable
hooks EVERY object of that class"). Gate G2 measures it: with the patch in,
count calls whose `this` is not ours; zero within ten seconds of play means
the table is not shared or the game reads elsewhere, and the door reports
"not reached".

Note the mechanism choice against `vtable_hook.h`'s own rule. That header
picks CopyVptr for a table inside the implementing module, because the
runtime re-points its own tables; here the shared table IS the mechanism --
CopyVptr on our dummy device would hook our dummy device and nothing else.
So this is InPlace, deliberately, with the header's costs accepted:
`reclaim()` runs on the frame path as for the context hooks, the `this`
check is in every thunk, and a foreign entry already in the slot is chained
through, not replaced. The Steam overlay is the foreign entry to expect:
BELIEVED to hook exactly these slots to block game input while it is shown;
the install log names which module each slot pointed at before EDVR
patched it (`dinput8.dll` on the Frontier install, presumably
`gameoverlayrenderer64.dll` on Steam's), which is gate G2's second half.

Only keyboard devices are filtered. The thunk asks each `this` once, through
the original `GetDeviceInfo`, whether it is `GUID_SysKeyboard`, and caches
the answer per object pointer (a small fixed table; an unknown object
passes through). A HOTAS, a throttle and pedals are DirectInput devices on
the same table and are never touched: **the ship keeps flying while the
menu is open**, which is a feature, not a limitation.

Two interfaces exist, `IDirectInputDevice8A` and `...8W`, with separate
tables. EDVR creates one device of each and patches both; the probe reports
which one the game's calls arrive on.

**Door 2 -- the user32 key-state trio.** `GetAsyncKeyState`, `GetKeyState`
and `GetKeyboardState`, patched in the game **executable's import table**
only (`src/common/iat_hook.h`, new: walk `GetModuleHandle(nullptr)`'s
import descriptors, find the user32 entries, exchange the pointers under
`VirtualProtect`, all-or-nothing with rollback, the discipline `vscreen_res`
uses for code patching). EDVR's own modules import these functions through
their own tables, so `hotkey.cpp`'s polling is unaffected and keeps reading
the real keyboard for the menu.

**Door 3 -- the message pump.** `PeekMessageA`, same IAT technique. When
the original returns a keyboard message (`WM_KEYDOWN`, `WM_KEYUP`,
`WM_SYSKEYDOWN`, `WM_SYSKEYUP`, `WM_CHAR`, `WM_SYSCHAR`, `WM_DEADCHAR`,
`WM_UNICHAR`) while the flag is set, the message is rewritten to `WM_NULL`
in place. No `WM_CHAR` is ever generated because no `WM_KEYDOWN` reaches
`TranslateMessage`. This is thread-agnostic, which is why it is preferred
over subclassing the window (`SetWindowSubclass` must run on the window's
thread, and whether the Present thread is that thread is unmeasured).
Side effect, accepted: Alt+F4 does not close the game while the menu is
open, since its `WM_SYSKEYDOWN` is nulled before `DefWindowProc` turns it
into `SC_CLOSE`.

### The policy: release everything, admit nothing

While the flag is set:

- `GetDeviceState` returns the device's real result code and a **zeroed
  buffer**: every key up. A key held at the moment the menu opens is seen
  released, which is what a modal dialog does and what lets the gate be
  used mid-thrust without leaving thrust latched.
- `GetDeviceData` returns the **key-up events and drops the key-downs**, so
  a key the game saw go down before the menu opened still gets its release
  and no internal toggle is left half-pressed.
- The user32 trio answers "up" for every key; `GetKeyboardState` zeroes its
  256 bytes.
- The pump's keyboard messages become `WM_NULL`.

The game never sees a chord it did not see start, and never sees a key
stuck down.

### The summon key is private too

The flag is not the only condition. The doors also swallow **the summon
key itself, whenever its chord is held, even with the menu closed** -- so
the game never sees the press that opens the menu, and the binding is
EDVR's alone. This costs one byte comparison per `GetDeviceState` call for
the whole session (the binding is pre-translated once, at configure, into a
DirectInput scan code with `MapVirtualKeyW(MAPVK_VK_TO_VSC)` plus the
extended-key bit for the cursor cluster, and stored as one packed word so a
live rebind cannot tear it). Where the translation is uncertain the swallow
is skipped for that key and the collision check below says so.

What this changes about Feature 4's collision check: it still reads the
player's actual bindings (`elite_binds`, extended with a reverse lookup
from key to element), but the warning now reads "your menu key masks Elite's
*X* while EDVR is installed" instead of "double-acts". A masked binding is
a milder failure than a double-acting one, and it is the player's own
choice of key that causes it.

### What is deliberately not captured

- **The mouse.** Never touched; the menu has no pointer.
- **DirectInput joysticks, throttles, pedals.** Never touched; the ship
  flies.
- **XInput pads, in v1.** `XInputGetState` is imported and IAT-patchable
  the same way, and phase B masks the d-pad and face buttons while the menu
  is open and leaves sticks, triggers and bumpers to the game. In v1 pads are
  neither watched nor masked.

### Fail-open, in five rules

1. **The gate follows the draw.** The flag may be set only while the openvr
   half has drawn the panel within the last few frames (the export bumps a
   stamp; the d3d11 half checks it before setting the flag). A menu that
   cannot be seen never takes the keyboard. If the openvr half is absent,
   mismatched or not calling the door, the menu refuses to open and logs
   once why.
2. **Every thunk is budgeted.** A fault inside a door drops that door to
   pass-through for the session (the `guardedBudget` pattern), and the menu
   footer and the Status page say "keys shared with the game" from then on.
3. **Idle dismiss always runs.** `menu.idle_dismiss` seconds (default 20)
   without a key or an aim change closes the menu and clears the flag, so a
   stuck condition costs at most that long.
4. **Escape and the summon key both close it,** and both are read by EDVR's
   own polling, which no door can affect.
5. **`menu.keyboard = shared`** turns the flag off entirely: the doors stay
   installed but pass everything, and the menu runs in Feature 4's original
   watched-keys mode, with the collision check as its safety.

### Why not the alternatives

- **A low-level keyboard hook** (`WH_KEYBOARD_LL`): process-wide, needs a
  message loop thread, is what `hotkey.h` refused for good reason, and
  BELIEVED not to block DirectInput's own reading of the device anyway.
- **A `dinput8.dll` proxy**: a third file in the game folder, a third slot
  to collide with other mods that use that slot as their loader, and it
  only helps if it loads before the game creates its devices. The shared
  table reaches a device that already exists.
- **`SendInput` or pose tricks**: injection, which this project does not
  do (the head-steer design's one exception is gated to a camera mode and
  still unbuilt).

## Navigation and interaction

Head-aim and keys coexist; whichever moved last owns the highlight.

**Keys, while the menu is open** (all private to the menu):

| Key | Does |
|---|---|
| Up / Down | move the highlight; hold to repeat (400 ms, then 12 Hz) |
| Left / Right | step the highlighted row: toggle, cycle a choice, step a number; hold to repeat; Shift steps a number by ten steps |
| Enter / Space | activate: toggle, next choice, or (phase B) open a number for typing; on an action row, fire it |
| Tab / Shift+Tab, PageUp / PageDown | previous / next page |
| Home / End | first / last row |
| R | reset the highlighted row to its shipped default (a confirm on the row, then R again) |
| Escape, the summon key | close (fade out, keys released) |
| the summon key with Shift | recentre: re-anchor the panel where you are looking now |

**Head-aim.** The head ray (from `headPose()`, the raw pose the openvr half
publishes every frame) is intersected with the panel in its anchor frame;
the row under it highlights, with hitboxes one full row pitch tall and a
hysteresis before the highlight moves, so a resting head never flickers it.
After a key press head-aim is parked until the ray leaves the highlighted
row's hitbox by more than one row. `menu.aim = keys | head | both`.
**Flown 2026-09-07: `both` fought the keys** -- a head that drifts back to
the row it was reading re-selects it under a hand that just moved away --
so the default is `keys`, and head-aim is an opt-in for a player who wants
it (gate G6's numbers would tune `both`, and are still unmeasured).

**Dwell-to-select** is off by default and stays a phase C item: it needs
an aim ray steadier than a head, which means the eye tracker, and
`docs/eye-tracking.md` records that no field driver publishes a usable one
today.

**Typing a value** (phase B, needs `menu.keyboard = private`): Enter on a
number row opens it for text; digits, `.`, `-` and Backspace edit; Enter
commits, clamped to the row's range and precision; Escape cancels. This is
the one interaction the gate makes possible that Feature 4 could not offer.

**Pads** (phase B): d-pad navigates and steps, A activates, B closes,
bumpers change page, watched through `xinput_watch` and masked from the
game through door 4.

## What it shows

Pages, in tab order. Each row is: label, value, a one-line hint (the
ini's own explanation, first sentence, by the generator's `summarise()`),
and where it applies, the measured cost or the restart badge.

1. **Performance.** The rows tagged `menu performance` in `edvr.ini`:
   `temporal_aa`, `ui_depth`, `render_sharpness`, `supersample_filter`,
   `foveation`, `foveation_centre`, and `render_scale` when its branch
   lands. Costs where they are measured: the temporal pass's own timing,
   NVIDIA's pass per eye, the sharpen's timestamp pair, the pixel fraction
   under scale.
2. **Fixes.** Every other `[fix]` row tagged `menu`, under the ini's own
   headings ("When the eyes disagree", ...), scrolling. Restart rows are
   shown, badged, and editable: the badge is the point of showing them.
3. **Monitor.** fpsVR's readout, gathered as cheaply as it can be, and
   where each number comes from:
   - frame rate, frame time, the 1% low (the 99th-percentile frame time)
     and the max, over the last ten seconds, from EDVR's own
     Present-to-Present clock, ringed every frame;
   - the app's GPU time and the compositor's, the CPU frame interval,
     dropped frames (the last ten seconds and since launch) and the
     reprojected and motion-smoothed shares, from **the compositor's own
     frame timing** -- `IVRCompositor::GetFrameTiming`, slot 8 of every
     generation this build knows, read once a frame at the WaitGetPoses
     boundary by the openvr half (`src/openvr/frame_timing.cpp`) and
     published on the channel; the first answer is validated before any
     is believed, OpenComposite's refusal is recognised, and
     `advanced.compositor_timing = off` turns the read off;
   - the display's rate and frame budget, and the eye size;
   - CPU load (system and Elite's share), RAM, VRAM through
     `IDXGIAdapter3::QueryVideoMemoryInfo`, and GPU load and temperature
     through NvAPI where an NVIDIA driver is present (elsewhere "n/a" --
     fpsVR's AMD path is a vendor library this project does not carry);
   - EDVR's own passes' measured cost, from the totals they already keep;
   - a **frame-time strip** of the last 120 frames against the budget
     line, green within it, amber over it, red at twice it.
   Its cost, by construction: the ring is one clock read and a store per
   frame; the compositor read is one small copy per frame; the load and
   memory samplers run once a second and ONLY while the page is showing;
   the page re-rasterises at 4 Hz on the worker thread. Frame time lives
   here now and not on Status.
4. **Status.** Read-only, the README's "checking it worked" as a live
   panel, and the page a support thread will ask for: EDVR's version and
   the game build; the runtime under the proxy (Valve's SteamVR,
   OpenComposite, or unknown, by the launch centre's export test); eye
   texture size and tangents; guard stage; temporal mode and whether
   NVIDIA's library loaded; the gate's state (which doors are armed, which
   the game has reached, keys private or shared); this panel's own price
   (bitmap size, raster time, the composite's measured GPU time per eye);
   the list of changes waiting for a restart; and the result of the last
   ini write.

**The overlay** (`menu.fps_overlay = on`, off by default): a one-line
readout -- frames per second and frame time over the last second, the
app's GPU time, frames dropped in the last ten seconds -- shown while the
menu is CLOSED and pinned to the head, the toolkit's overlay, because that
is what was asked for and a gauge you carry has its uses. `fps_overlay_yaw`
and `fps_overlay_pitch` put it where you can stop seeing it (default 16
degrees below the look). The lock has no lag: the door builds its anchor
from each frame's own pose (`setMenuHeadLock` on the channel) rather than
from a value published a frame earlier. Its price is the panel's: one
region copy plus a composite over the readout's own pixel box per eye per
frame, timed by a timestamp pair and printed in the graphics log after 240
frames (`menu panel: measured ... ms per eye`), so the number is measured
and not believed.

With `menu.developer = on`, three more pages and two changes everywhere:
the ini's dotted key name appears under each label, and each row's hint
gains the "live" or "restart" word the generator derived.

4. **Advanced** and 5. **Experimental.** Every key the build reads from
   those sections, automatically: `getBool` reads are toggles, `getInt` /
   `getFloat` (and their `InRange` forms) are numbers with the declared
   bounds, and `getString` reads are read-only unless the key carries a
   `# dev: choices a, b, c` line (a new, optional annotation, allowed only
   outside `[fix]`, the mirror of the rule that `# ui:` is allowed only
   inside it). Instruments that write files or scan memory are still one
   toggle away, which is why the tier exists and is off by default.
6. **Instruments.** Action rows for the things that today need a hotkey
   bound: dump the camera history (the `PAUSE` key's job), take a draw
   census with the quad probe, reload `edvr.ini` now, write a marker line
   to both logs, and reset Explorer Cam's counted view to zero (the planned
   camera-index reset, which has needed a key of its own and gets a row
   instead). Each fires the same function its hotkey fires.

**Toasts.** When a live setting changes -- from the menu, the installer's
window, or a hand edit -- one line fades through the view for a couple of
seconds: `sharpening 0.30 -- 0.19 ms`, or `vr_handover = early -- takes
effect at the next launch`. Not head-locked: a toast spawns where you are
looking at that moment, low in the view, and stays there while it fades.
With the menu open, toasts are unnecessary (the row itself updates) and are
suppressed. `menu.toasts = off` for players who want nothing uncommanded
on screen, ever; the menu itself never appears uncommanded.

## Restart flagging

Sean's addition, made a first-class mechanism rather than a badge:

- **Derived, not declared twice.** `tools/gen_settings_schema.py` already
  reads "live" or "restart" out of each setting's prose, or from `| live`
  / `| restart` on the `ui:` line (`when_it_applies()`). The menu's table
  is generated from the same pass, so a row can never say something the
  ini does not.
- **Enforced at build time.** Today the generator already fails the build
  when a `[fix]` setting shown in the desktop window says neither word
  (`gen_settings_schema.py`, "do not say when they take effect"), and
  every `[fix]` row the menu shows is one of those. For the developer tier
  -- the `[advanced]` and `[experimental]` keys, which were never checked
  -- the generator prints a WARNING naming each key that does not say, and
  the row wears a `?` badge with "when it applies is not documented" in
  its hint, so an unknown is never silently read as live. (As built: 61
  such keys on 2026-09-07; the error form waits until their comments are
  written, which is a separate change.) A restart row that reads as live
  is the failure this exists to prevent.
- **Shown three ways.** The row wears a `restart` badge always. After a
  change it shows the running value and the pending one (`stock -> early
  at next launch`). The panel footer counts them ("2 changes take effect
  at the next launch"), and the Status page lists them by name.
- **Known, not guessed.** At its first parse the d3d11 half snapshots the
  values of every restart key (the generated table says which). Any later
  parse -- a menu write, a hand edit, the installer -- diffs against that
  snapshot; a difference is a pending change. This also gives the config
  audit a line it has always lacked: `edvr.ini: advanced.projection_edit
  changed on disk but is read at launch; the running value is still on`.
  That line ships with this work whether or not the menu is open.

## Placement and rendering

**Anchor.** On summon, the d3d11 half latches the current raw head pose
(`headPose()`, published by the openvr half before any EDVR offset touches
it) as the anchor and publishes it on the channel; the panel sits
`menu.distance` metres (default 1.4) along the anchor's forward, upright
in the anchor frame, and does not move until recentred or re-summoned. At
the door, the openvr half computes the per-eye transform exactly as
`theaterXform` does -- current-head vectors into anchor space, then the
eye's ray origin with the eye's real lateral offset from
`GetEyeToHeadTransform` rather than the theater's constant -- and hands it
to the export. Unlike the theater, the game is fed the LIVE pose
throughout, so the compositor's reprojection is correct for the panel and
the frame alike; nothing here needs the theater's world-lock reasoning
because nothing here lies to the runtime. Under `pose_hold` or the retired
`shimmer_rest`, whatever pose the submit carries is the pose the export is
given (gate G7).

**Where at the door.** Last, as [anti-aliasing.md](anti-aliasing.md)'s
"order at the door" already reserves: after the temporal pass, the crop,
the scale and the sharpen, onto the native-size outgoing frame. If that
frame is already an EDVR texture (any of those passes ran) the panel is
drawn straight onto it, zero extra copies. If it is the game's own texture
the door first takes the per-eye copy the resubmit shadow already knows how
to make, draws on the copy, and submits that. The game's textures are never
drawn on.

**The draw** (as built): a compute pass in the FSS theater's shape rather
than a quad -- per output pixel, the view ray from the published tangents
is rotated into the anchor's frame and intersected with the panel, flat or
on a cylinder (curve `menu.curve`, default 0.2), and the bitmap is blended
over the frame's pixel. The eye's region is copied into an EDVR-owned
texture and the dispatch covers only the panel's projected bounding box
(nine points along its top and bottom edges, so a curved panel's bulge is
inside it); a panel entirely outside an eye costs that eye nothing at all.
No depth: the panel draws over the cockpit like the game's own HUD does. A
150 ms fade in and out is the only animation. The shader is embedded HLSL,
compiled on first use.

**Type in degrees.** Cap height `menu.text_degrees` (default 1.1, gate G5
measures it), row pitch twice that, the panel about 24 by 18 degrees. The
bitmap is sized per headset from the eye size and tangents the channel
already carries, at the headset's own pixels per degree at the panel, so
it composites near 1:1 and is native-crisp whatever `render_scale` says.

**Rasterisation.** GDI into a 32-bit DIB section: `CreateFontW` /
`DrawTextW` with `ANTIALIASED_QUALITY`, white on black, luminance taken as
alpha -- the installer's own text path (`src/installer/ui.cpp`), and the
game already imports GDI32, so nothing new enters the process. Redrawn on
**change only**, on a worker thread, into the back half of a double-buffered
texture that the frame thread swaps in; per frame the panel costs one draw
per eye and nothing on the CPU. DirectWrite would give better glyph shapes
and a build-time SDF atlas would remove the font engine entirely; both are
noted as the upgrade if GDI's output disappoints at 1.1 degrees (gate G4),
and neither is needed to start.

## Persistence

The menu writes `edvr.ini` and nothing else; the running configuration
changes because the file did.

- **The installer's own edit.** `iniedit.cpp` moved from `src/installer`
  to `src/common` (it already followed `config.cpp`'s grammar exactly and
  had no installer dependencies; it now lives in the plain `edvr`
  namespace, which the installer's own namespace finds unqualified); the
  d3d11 half uses `mergeIni(source, source, &source, {{dotted, value}})`
  for one value, exactly as `SettingsModel::set` does -- the line where
  the key already lives, uncommented if it was an expert default, every
  comment untouched.
- **Re-read before write**, the installer's 2026-08-28 lesson: the file on
  disk is the source, never a cached copy.
- **Atomic write**: temp file beside the ini, then `MoveFileExW` with
  replace-and-write-through, so `config.cpp`'s size check never meets half
  a save and an editor holding the file mid-save cannot lose the write.
- **Apply now**: `Config::get().reloadIfChanged()` is called immediately
  after the write, so the change lands on this frame instead of the next
  poll, and through the same configure path a hand edit takes -- no module
  learns anything new.
- **Backup once per session** before the first change, to
  `edvr_backup\menu-<stamp>\edvr.ini`, the settings window's rule.
- **Mirror.** After each write the ini is copied to the installer's mirror
  (`%LOCALAPPDATA%\EDVR\<leaf>-<store>\`), so a game update that wipes the
  folder cannot lose an evening's tuning; `mirrorDirFor`'s naming rule
  moves to common with `iniedit` so the DLL and the installer cannot
  disagree about the folder.
- **Log line per change**: `menu: fix.render_sharpness 0.0 -> 0.3 (written;
  live)` or `(written; takes effect at the next launch)`.

The installer's settings window and the menu can never disagree, because
neither owns any state the other lacks; the desktop window remains the
place for everything, the menu the flight-relevant subset plus, for
developers, the rest.

## Safeguards, gathered

It never appears uncommanded (toasts excepted, and they have an off
switch). It never takes the keyboard unless it is being drawn. It draws
only onto EDVR's own copies. A rasterisation or draw failure means no menu
and one log line, with the game unaffected. A door fault means keys shared
and the panel says so. Closed, it costs one key poll and one byte
comparison per DirectInput call; open, it says its own price on the Status
page.

## Settings sketch

```
[hotkey]
# Summon and dismiss the settings menu. Private to EDVR: the game never
# sees this key while it is bound here. Checked against your own Elite
# bindings at launch and on every rebind; a clash is named in the log and
# on the panel. Live.
menu = F8

[menu]
# While the menu is open the game sees no keyboard at all (private), or
# every key reaches the game as well and the menu only watches (shared).
# Live.
keyboard = private

# How the highlighted row is chosen: the arrow keys (keys), where your
# head points (head), or whichever moved last (both). Live.
aim = keys

# The head-locked readout while the menu is closed, and where it sits.
fps_overlay = off
fps_overlay_yaw = 0
fps_overlay_pitch = -16

# Metres from your head to the panel, and how much it wraps toward you.
# Live.
distance = 1.4
curve = 0.2

# The height of a capital letter, in degrees of your view. Live.
text_degrees = 1.1

# Close after this many seconds without a key or a look; 0 stays open
# until dismissed. Live.
idle_dismiss = 20

# One-line confirmations when a setting changes outside the menu. Live.
toasts = on

# Adds the Advanced, Experimental and Instruments pages, and shows each
# setting's ini name. Everything on those pages is a safety valve or a
# developer instrument; the log names one when it wants you to change it.
# Live.
developer = off
```

The `[fix]` rows opt in on their existing `ui:` line: `# ui: Sharpening |
range 0..1 | percent | menu performance` puts a row on the Performance
page; a bare `| menu` puts it on Fixes. A `[fix]` row with no `menu` token
stays in the desktop window only.

**The summon default, F8, comes from a measurement** (2026-09-07, all 30
`.binds` files under the game's `ControlSchemes\`): across every stock
scheme, 75 bare keyboard keys and 7 chords are used (`CTRL+ALT+SPACE`,
`CTRL+SPACE`, `SHIFT+W/A/S/D`, and one pad chord); the whole cursor
cluster (Insert, Home, End, Delete, PageUp, PageDown) is taken by the thrust
bindings, and the Pause cluster is already EDVR's (`SCROLLLOCK`, `PAUSE`,
`NUMLOCK` earmarked). F2 through F9 are bound in no stock scheme; F1 is,
F10 and Alt+F10 are the game's hard-coded screenshot keys, F12 is Steam's,
Escape is the game menu. F8 is layout-independent and findable blind in
the F5-F8 group. Because the press is swallowed, a player who has bound F8
themselves loses only that binding, and the collision check says so at
launch. `CTRL+ALT+<letter>` would also be free (only Space is chorded that
way in stock) for anyone who prefers a chord.

## Architecture

**d3d11 half** (new files, `src/d3d11/`):

- `menu_model.cpp` -- pages, rows, highlight, the generated table
  (`menu_schema_gen.h`, a second output of `gen_settings_schema.py`
  carrying every section with its tier, kind, bounds, choices, applies and
  page), the restart snapshot and pending list.
- `menu_input.cpp` -- the summon key through `Hotkey`, the navigation keys
  through the same edge-and-repeat polling, head-aim from `headPose()`
  against the published anchor, idle timing through `timing.h`.
- `menu_raster.cpp` -- GDI DIB rasterisation on a worker thread, the
  double-buffered texture, the toast bitmap.
- `menu_ini.cpp` -- the write path above, on top of `common/iniedit`.
- `input_gate.cpp` -- the doors: the dinput8 dummy devices and their
  in-place vtable patches (`VTableHook`, InPlace, `reclaim()` from the frame
  path), the exe-IAT patches (`common/iat_hook`), the summon-key scan code,
  the input probe.
- The export `edvrMenuPanel(outTex, eye, tangents4, xf12, alpha)`: draws the
  current panel and toast onto `outTex`. Bumps the "drawn" stamp the gate
  checks.

**openvr half** (`compositor_hook.cpp`, the door lambda): reads the
channel's menu flag and anchor, takes the shadow copy if the outgoing
texture is the game's, computes `xf` per eye as the theater does with the
real eye offset, calls the export, submits what came back. Publishes one
new word: the runtime kind, for the Status page.

**Channel** (`frame_flag.h`): `publishMenuAnchor(m12)` / `menuAnchor()`,
`setMenuVisible(bool, alpha)` written every frame (a heartbeat, the
`externalCameraOnFoot` discipline, so "closed" and "d3d11 stopped saying"
stay distinguishable), `bumpMenuDrawn()` / `menuDrawnValue()` the other way,
and `announceRuntimeKind(k)`.

**Common**: `iniedit` and `mirrorDirFor` move in; `iat_hook.h` is new.

**Tests** (`tools/`): `menu_test` drives the model through keys and aim
with the test clock (rate-invariant repeat and idle, hysteresis, the
pending-restart diff); `gate_test`-style fixtures for the doors against a
fake `IDirectInputDevice8` vtable (the filter policy: zeroed state, ups
kept, downs dropped, non-keyboard devices untouched, foreign entry chained)
and against a synthetic import table; `installer_test` already proves the
one-value merge and gains the atomic-write case.

## Phase 0 -- what must be measured before code depends on it

1. **G1, the input probe** (`advanced.input_probe = on`, an instrument in
   the gaze probe's style): with the doors installed pass-through, count
   per five seconds the game's calls to `GetDeviceState` and
   `GetDeviceData` (A and W), to each of the user32 trio, and the keyboard
   messages seen by `PeekMessageA`, plus any `WM_INPUT` (which would mean a
   dynamic Raw Input registration the import table hid). Decides which
   doors matter and whether one is missing.
2. **G2, reach and attribution.** The dummy-device table patch is reached by
   a `this` that is not ours within ten seconds of play (else "not
   reached", and the menu runs shared). The install line names the module
   each slot pointed at before the patch, on both installs; the Steam
   overlay's presence in the chain is the expected finding there.
3. **G3, the modal test.** Hold a thrust key, summon: thrust stops. Close
   with the key still held: thrust resumes. Type in the galaxy map search
   with the menu open: nothing lands. A key that leaks names the fourth
   path (the game's own `SetWindowsHookExA`) and gets a door of its own.
4. **G4, GDI beside the game.** Cost of one full re-raster at the Pimax's
   bitmap size (expected 1-3 ms, off the frame thread), no loader
   surprise at first use, and whether the glyphs satisfy at 1.1 degrees.
5. **G5, legibility in degrees**, both rigs -- performance.md's item 9.
6. **G6, head-aim comfort** -- item 10.
7. **G7, anchoring.** The panel holds under head translation and rotation
   on both rigs; it holds under `pose_hold`; the FSS theater and the menu
   coexist; the menu over the loading screen and the main menu.
8. **G8, the collision check.** The stock sweep is done (above); the live
   check runs against a real player's binds file with F8 deliberately
   bound in Elite, and the panel shows the warning.
9. **G9, the write round trip.** Write, immediate reload, mirror refreshed,
   the installer's window reopened reading the same value; a hand edit
   while the menu is open lands on the panel within the poll; the atomic
   replace never trips `config.cpp`'s size check.
10. **G10, fail-open.** Force the export to fail (a debug key): the flag
    clears within the stamp window and the game has its keyboard back; a
    forced fault in a door retires that door only.
11. **G11, focus.** Alt-Tab away with the menu open: idle dismiss closes it;
    the overlay (Shift+Tab) opens and closes cleanly with the menu up.
12. **G12, the restart snapshot.** Change a launch-time key by hand and by
    menu; the pending list, the footer count and the new audit line agree.

## Phasing

- **A, the menu** (this design's v1): the three doors, the model, pages
  1-3, head-aim plus keys, toasts, the ini write path, restart flagging
  with its build gate, the developer tier with pages 4-6. Ships default
  on with `keyboard = private`, because the whole point is that it is safe
  to.
- **B, the extras**: typing a value, pad navigation and door 4, the
  performance monitor's "move it here" row (Feature 5), per-row measured
  costs as each pass gains a timestamp pair.
- **C, gaze**: aim by eye and dwell-to-select, the day a driver publishes a
  usable centre; the highlight then becomes the tracker's live sanity
  check, as Feature 4 hoped.

## Open questions for Sean

1. **F8 as the summon default**, or a chord? The sweep says either is free;
   a single key is easier blind.
2. **`keyboard = private` by default.** The design assumes yes; `shared` is
   there for a rig where a door misbehaves.
3. **`developer` under `[menu]`** rather than `[advanced]`, since it changes
   what the menu shows and nothing else. And whether the Instruments page
   should fire the dev hotkeys' functions at all, or only show them.
4. **Which `[fix]` rows get the `menu` token** in the first cut. The
   proposal above is the Performance list plus every live-editable fix
   with a choice or a toggle; the metre offsets stay desktop-only as the
   generator's comment argues.
