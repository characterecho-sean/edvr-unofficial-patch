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
3. **Idle dismiss, when it is set.** `menu.idle_dismiss` seconds without a
   key or an aim change closes the menu and clears the flag. It SHIPS AT 0
   -- the menu stays up until the summon key or Escape puts it down, which
   is what a settings panel should do (asked for 2026-09-07) -- so this is
   a belt for someone who wants one, not the safeguard. The safeguards
   that always run are the two below: the draw stamp and the two keys.
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
| Enter / Space | activate: flip a switch, next choice, or open a number or string for typing; on an action row, fire it |
| Tab / Shift+Tab | next / previous page |
| PageUp / PageDown | read on through the explanation beside the row, three lines at a time; changes the page when there is nothing to scroll |
| Home / End | first / last row |
| R | reset the highlighted row to its shipped default (a confirm on the row, then R again) |
| Escape, the summon key | close (fade out, keys released); Escape first abandons a value being typed |
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

**Typing a value** (BUILT 2026-09-07, the gate's whole point): Enter on a
number or string row opens it for typing. The digits, the letters, the
numeric pad, `.` `-` `,` space and Backspace edit the buffer -- and
nothing else, so a stray key cannot corrupt a value; Enter writes it,
Escape leaves it alone, and moving off the row abandons it. A number is
checked before it is written: one that does not parse, or falls outside
the row's own range, is refused with the range named on the Status page's
last-write line rather than written and clamped silently. While a row is
being typed the arrows and Tab belong to the editor, and head-aim is
parked, so a look that wanders cannot take the row away mid-value.
Typing is what the keyboard gate makes possible and Feature 4 could not
offer.

**Anything with two states is a switch.** A `Toggle` row draws the
installer's control where its value would be -- an accent track with the
knob at the end the value is at -- and so does a two-way CHOICE where one
side is "leave the game alone", by the installer's own rule
(`twoChoiceToggle` in `settings_view.cpp`): exactly two choices, one of
them `off` or `stock`. That is most of the fixes, and they read as
switches now rather than as words to cycle. Where the value written is
not literally `on`/`off` -- one that writes `steady` against `stock` --
the word stays beside the switch, because the switch alone cannot say
it; where it is, the word goes. A row with a pending restart change keeps
its text (`off -> on`), because a switch cannot show two values at once.

**The tooltip is a card of its own, BESIDE the panel.** The highlighted
row gets what `edvr.ini` says about that key. **The facts come first** --
the range or the list of choices, the shipped value, whether the change
applies at once or waits for a launch, and the key that acts on it --
and the ini's own comment block follows. That order is deliberate: the
buffer holds about a thousand characters and forty-six of the comment
blocks are longer than that (the longest is three and a half thousand),
so something is cut on those rows, and what is cut must be prose and
never a fact. The generator carries the full comment block into the row
table as `detail` for this.

It appears only after `menu.tooltip_delay` seconds resting on one row
(1.5 by default, 0 for never): an explanation is for someone who has
stopped, not something to flick past. It is set in a face smaller than
the rows, and its card is sized to the MEASURED height of its text --
`DT_CALCRECT` with the same font and the same wrap the draw will use,
not a character count -- capped by the panel's own height. A rule joins
it to the row it belongs to. Resting the look on the card counts as
using the menu, so an idle dismiss set by hand cannot close the panel
mid-sentence.

**What does not fit is scrolled to, not lost.** PageUp and PageDown move
the body three lines at a time, with a thumb on the card's edge showing
how much there is and where in it you are. The raster measures the text
and publishes how far it can still go (`menuPanelPopupScrollMax`), and
the model clamps its counter to that, so the scroll cannot run off the
end of a text only the raster has measured. Those two keys fall back to
changing the page when there is nothing to scroll; Tab is what changes
the page deliberately.

**How it sits beside the panel without moving it.** The rasterised bitmap
is WIDER than the menu card -- the card, a gap, and the tooltip's strip,
the last two a fixed fraction of the card (`kTipGapFrac` and
`kTipWidthFrac`, in `menu_panel.h` because both the model and the raster
must read the same numbers) -- and the strip is transparent whenever no
tooltip is up. Because the strip is always allocated, the panel's size in
the world never changes as a tooltip comes and goes, or between pages.

The bitmap is then **slid along its own surface** by `panelShift()`, so
that the CARD's middle lands on the anchor's forward rather than the
bitmap's. One value reaches the shader, the hit test and the culling box,
and the card comes out exactly as it was before the strip existed: the
same width, the same distance, square to the look.

Two things were got wrong on the way here and are worth keeping written
down. The first build **turned the anchor** instead. That put the card's
middle in the right direction but left it facing the old one, so the card
was seen nine degrees oblique, its left edge 1.50 m away and its right
1.41 m; and being latched at summon, it went stale the moment the ini was
edited with the panel up. The second mistake was scaling the panel's
angle by the strip's ratio: the bitmap maps linearly onto the surface and
the tangent does not, so the card came out 3.6% wider than
`width_degrees` asked, and 9% at the widest setting. Both are why
`panelHalfW()` works in metres and `panelShift()` is recomputed every
frame.

The head-aim hit test works in the bitmap's own coordinates and
`menuPanelLineAt` rejects any point in the strip, so a look parked on the
tooltip selects nothing rather than the row at that height. The toast and
the fps overlay share the raster and get no strip and no shift.

With the strip, the panel reaches `atan(2.14 * tan(w/2))` to the right --
29.8 degrees at the default width. Past about 35 the strip leaves one
eye's frustum on most headsets and the card would be seen by one eye
only, which is the worst thing to do to text somebody has stopped to
read, so `menu.width_degrees` is held to 36 while tooltips are on and the
log says once that it was.

The very first build drew the card over the rows, and it hid the values
it was explaining (flown 2026-09-07).

**Pads** (phase B): d-pad navigates and steps, A activates, B closes,
bumpers change page, watched through `xinput_watch` and masked from the
game through door 4.

## What it shows

Pages, in tab order. Each row is: label, then its value as a switch, a
number, a choice or a typed string, and where it applies, the restart
badge. The ini's own explanation is in the tooltip beside the row.

**The tab strip scrolls.** It shows the window of pages that fits the
panel's width, always including the current one, with a `<` or `>` at
whichever end has more. Developer mode adds four pages and the strip ran
off the edge -- the pages past it could not be seen, and nothing said
they were there (flown 2026-09-07).

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
   - **two frame-time strips**, fpsVR's pair: the GPU frame and the
     render thread's busy time, each the last 120 frames against its own
     budget line, green within it, amber over it, red at twice it. A
     frame over budget on the GPU strip is a different problem from one
     over budget on the CPU strip, which is why they are drawn apart; a
     frame whose compositor record has not settled is a gap in the GPU
     strip rather than a zero-height bar.
   **GPU TIME and CPU TIME are fpsVR's two frametimes.** GPU TIME is the
   compositor's own GPU frame total -- its record's "time between work
   submitted immediately after present until the end of compositor
   submitted work", which is the timeline fpsVR's author describes as the
   scene, the companion window and the distortion pass. CPU TIME is
   EDVR's own measurement, not the compositor's: the Present-to-Present
   period less the time the game's thread spent blocked inside
   WaitGetPoses and inside Present, which is the render thread's busy
   time. The APP GPU tile carries the app's share, the frame total less
   the compositor's, and names the record's own two app fields beside it.
   The Present period sits on the FRAME RATE tile; frame rate, the 1%
   low and the strips stay on the period, as fpsVR's do.
   **Both are the mean over about 200 milliseconds**, which is fpsVR's
   own update window, with the ten-second mean on the sub-line. Averaging
   the whole ring instead read about a millisecond under fpsVR (flown
   2026-09-07), and that is the whole of that gap.
   A trap worth recording: fpsVR's PINNED Q&A defines both readouts as
   the "Maximum ... per fpsVR refresh interval", and that page is wrong
   today. It describes the behaviour before fpsVR 1.24.1 (August 2022),
   which changed the printed numbers to the arithmetic mean over one
   overlay update; the Q&A was never edited. The graphs stayed
   per-frame. Do not take that page as the answer a second time.
   **The record read is the settled one, two compositor frames back.**
   Flown 2026-09-07 with the most recent record (`framesAgo = 0`): its
   GPU fields had not resolved at the WaitGetPoses boundary. Two further
   findings from the probe on the next flight:
   - **The 176-byte layout is not this header's.** The reader offered
     176 bytes first and SteamVR ANSWERED, but the record it filled puts
     `m_flSystemTimeInSeconds` where this struct has the dropped-frame
     count and the reprojection flags -- proven by decoding those two
     words as one double: 33699 s and 33740 s at two probes 40.7 s
     apart. Every drop the page counted from that record was noise, and
     the drop-frame log line fired on frames that never dropped. The
     reader now offers **184, openvr.h's own, first**, and if only 176
     is accepted it says so and leaves the count and reprojection
     columns blank rather than showing numbers it cannot decode.
   - **Elite's WaitGetPoses is thirty microseconds before its Submit.**
     The record's poses-ready and frame-ready stamps read 1.14 and 1.17
     ms from the same vsync, every frame: the game renders, then latches
     poses, then submits. So the compositor's app-busy window is ~0 for
     this game and cannot be a CPU frametime, which is why CPU TIME is
     measured here instead.
   The monitor writes the settled record into the ring entry of the frame
   it describes, two back, so drops and EDVR's events line up. Twice a
   session (20 s and 60 s after arming, three frames each) the openvr
   half logs the four most recent records side by side, every decoded
   field, as "compositor timing probe", followed by the most recent
   record's raw words in hex -- so a layout this build decodes wrongly
   can be read off any flight's log without another build.
   Laid out as **sixteen tiles, four across** -- a caption, one big
   number, one small line each -- after the first flight found rows of
   sentences full of numbers unreadable in a headset; the last drop and
   EDVR's events in it are the one line under the tiles. The second
   flight found the big number clipped and the sub-lines running off
   the tiles: a four-across tile is about seven degrees wide, sixteen
   characters of a small face, and the number's box had been sized to
   its cap height rather than its line box. Now every box is sized to
   its font's line height, the sub-line wraps over two lines in a
   smaller face, and no sub-line is longer than about thirty characters.
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

**Diagnosing drops caused by the mod** (built 2026-09-07 the same night,
after the question was asked). A drop count says drops happened, not why,
and the mod's own one-off work is the class that would explain one: a
shader compile at first engage, a withhold, a reload parsing the ini on
the render thread, the menu's own write. So every frame's ring entry now
carries what EDVR did in it and what it cost, and the page and the log
read the two together:

- **Events**, ORed into the frame from wherever they happen, both halves
  (`PerfEvent` in `perf_monitor.h`, the openvr half's crossing on the
  channel): a reload with its duration, an ini write, a shader compile
  with its duration (every `shaderSwapCompile*`), NVIDIA's feature
  creation with its duration, a withhold, a resubmit, a census request, a
  bitmap upload, a bindings re-read, the menu opening or closing.
- **EDVR's CPU time**, measured: the frame boundary's body (a clock around
  `hookedPresent`'s frame work), the door's passes per eye (a clock around
  the lambdas in `hookedSubmit`, crossing on the channel), and the draw
  hooks on one frame in sixteen -- the four draw thunks clock themselves
  and the real call they forward, and the difference is EDVR's own cost in
  the hook. Two clock reads per draw on a sample frame, one branch on the
  other fifteen.
- **EDVR's GPU time at the door**: a timestamp pair per eye around every
  pass the door runs (`edvrDoorGpuBegin` / `End`, exports the openvr half
  calls), never awaited, polled on later calls. This is the mod's whole
  submit-side GPU price in one number, beside the compositor's app GPU
  figure. What it does not cover: the ui_depth second draws and the
  theater's draw-path work, which happen inside the game's frame.
- **The page**: "Dropped frames" now ends with how many of the window's
  drops coincided with EDVR activity, naming the events, against how many
  were clean; "Last drop" shows the most recent drop or long frame with
  its interval and EDVR's events in it; "EDVR CPU" and "EDVR GPU" show the
  measured per-frame figures.
- **The log**: a `monitor: DROPPED FRAME` (or `LONG FRAME`, for one over
  twice the budget by EDVR's own clock when the compositor reports no
  drop) line with the interval, the compositor's record for the frame
  (GPU app and compositor, the app's busy time, when the poses came and
  when the submit landed, from the vsync), EDVR's four costs and its
  events, at most one every five seconds and sixty a session -- so a
  field report carries the attribution without the headset on.
- **The frame the record describes.** The compositor's record is read
  two frames back (the settled one, above), and it is written into the
  ring entry of THAT frame, beside the events EDVR raised in it. The
  first build wrote it into the newest entry, so a drop was blamed on
  whatever EDVR did two frames after it -- and with the Monitor page up,
  that was its own bitmap upload four times a second, which is why the
  "with EDVR" count climbed steadily on the second flight (2026-09-07)
  while every logged drop said "EDVR events: none".
- **And the hitch removed**: the menu's ini write (read, merge, write,
  replace, mirror copy, backup copy) ran on the render thread in the first
  build; it now runs on a worker, serialised and coalesced while a key is
  held, and the frame thread only enqueues, shows the value, and drains
  the results.

What this cannot do is name a cause the ring does not carry. A drop with
no EDVR event and ordinary EDVR costs is the game's or the runtime's, and
the page says so by calling it clean.

**The overlay** (`menu.fps_overlay = on`, off by default): a one-line
readout -- frames per second over the last second, the GPU frame time
from the settled records in it, the render thread's busy time, frames
dropped in the last ten seconds -- shown while the
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
   bounds, and `getString` reads are typed unless the key carries choices.
   A developer key gets its choices from a `# dev: choices a, b, c` line,
   or from a full `# ui:` line where it has one.
   **A `ui:` line is allowed outside `[fix]`** (`UI_SECTIONS` in the
   generator). It buys the key its LABEL and its CHOICES in this menu and
   nothing else: the installer's window still shows only `[fix]`
   (`EXPOSED_SECTIONS`), and the key's page and tier still come from its
   section. The point is that demoting a setting out of `[fix]` costs it
   its tier and its page but not the words somebody already wrote for it.
   Foveated shading was demoted that way on 2026-09-08 -- it ships off,
   and what it saves does not move the frame rate on Elite -- and kept
   both its label and its four presets. A key may carry a `ui:` line or a
   `dev:` line, never both.
   Instruments that write files or scan memory are still one toggle away,
   which is why the tier exists and is off by default.
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

**Anchor.** On summon, the d3d11 half reads the current raw head pose
(`headPose()`, published by the openvr half before any EDVR offset touches
it), takes its look direction's yaw and pitch and builds the anchor from
those with **no roll** -- upright in the world, so the panel's edges are
level whatever tilt the head had at the summon (the first build latched
the whole pose, roll included, and a head cocked at F8 got a cocked menu;
flown 2026-09-07) -- and publishes it on the channel; the panel sits
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

# The height of a capital letter, and the panel's width, in degrees of
# your view. Live.
text_degrees = 1.1
width_degrees = 30

# Close after this many seconds without a key or a look; 0 stays open
# until dismissed, which is what it ships doing. Live.
idle_dismiss = 0

# Seconds resting on a row before its explanation appears beside the
# panel; 0 never shows one. Live.
tooltip_delay = 1.5

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
