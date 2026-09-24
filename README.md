# EDVR — an unofficial patch for Elite Dangerous: Odyssey in VR

Two dozen fixes for things that make Odyssey uncomfortable in a headset, in a
native OpenXR package that takes about three minutes to install. The short list
is under [What it fixes](#what-it-fixes), and [docs/fixes.md](docs/fixes.md)
describes each one in full.

EDVR's Windows release runs Elite on native OpenXR. Elite still starts VR
through its OpenVR-facing path, and EDVR answers there, using the bundled
Khronos loader to reach whichever runtime Windows has set as its active OpenXR
runtime. SteamVR is one valid choice. Elite's legacy LibOVR path is not
supported, even as a fallback.

If something is not working, [open an
issue](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/new/choose).
Bugs get fixed there because you can attach the log, and the log usually shows
the cause. For setup questions, "is this normal", or just talking about it,
there is a [Discord](https://discord.gg/ynkdf6Gdua).

EDVR is free and stays free. If it improves your VR experience, [tips are
welcome](https://ko-fi.com/seancharacterecho), but please do not feel any
obligation.

> **Already running EDHM or ReShade?** Both run alongside EDVR. EDHM also
> installs itself as `d3d11.dll`, and only one file can have that name, so
> don't overwrite it. The installer handles this for you; by hand it takes one
> rename and one setting, described under [Running alongside other
> mods](#running-alongside-other-mods). ReShade has needed nothing at all since
> 0.7.2.

## Headsets and VR runtimes

EDVR's native OpenXR route has been checked in the field on Pimax over
PiOpenXR, Quest 3 over Virtual Desktop's VDXR, and the latest Air Link flight.
Nothing is claimed for other headsets and runtimes.

EDVR keeps Elite's OpenVR-facing entry point for compatibility, and behind it
EDVR's native runtime owns the OpenXR session. The bundled loader can select
SteamVR, PiOpenXR, VDXR or Meta's OpenXR runtime, and all of them run on the
same native EDVR backend. The package has no backend to choose and never falls
back to the legacy EDVR pair. The release installer checks the Elite
executable's profile and refuses an unsupported game revision before it writes
anything.

Before launching Elite, set the Windows active OpenXR runtime to the one you
intend to use, since that is the one the bundled loader picks, and make sure
that runtime is available and your headset connected. EDVR needs no SteamVR
loader; do not install OpenComposite or a vendor OpenXR loader to make it work.
Then launch Elite as you normally would.

To see which runtime you are on, look in `edvr_logs\` next to the game after a
session:

| What you find | What it means |
|---|---|
| Native OpenXR startup and runtime name | The bundled loader reached the Windows Active Runtime. |
| Unsupported profile error | This game revision needs an updated EDVR profile. |
| No native startup log | Native startup was not confirmed; attach whatever logs there are. |

## Install

**Run `edvr-installer.exe`** from the release. It is a single file, with
nothing to extract or put in the right folder, and it carries the native
graphics/runtime pair, the bundled OpenXR loader and its notice, `edvr.ini`,
and the optional DLSS runtime. It:

- finds the game, whether it came from the Frontier launcher, Steam or Epic, on
  any drive. It asks each launcher where it put the game and confirms each
  answer by finding `EliteDangerous64.exe`. You can also point it at a folder
  yourself.
- places the bundled OpenXR loader beside the native runtime, where it selects
  Windows' Active Runtime. It does not search for or load a SteamVR or vendor
  loader.
- keeps your settings. Updating never overwrites `edvr.ini`: it writes the new
  version's file with your values put back into it, so new settings and changed
  defaults arrive and nothing you tuned is lost. Afterwards it tells you which
  is which.
- backs up the game's original `openvr_api.dll` so that uninstalling can
  restore it. If that original is missing, it still does not fall back to
  anything.
- leaves other mods working. If EDHM or anything else is already installed as
  `d3d11.dll`, it renames that file aside, takes the name, and sets
  `advanced.real_dll` so EDVR passes every call through to it.

It also has these buttons and screens:

- Repair, for when another mod's installer overwrites EDVR's files. A common
  case is EDHM's uninstaller running `del d3d11.dll`, which after an EDVR
  install deletes *ours*. Repair puts both back, side by side.
- Uninstall, which renames back everything it renamed.
- A settings screen listing every setting with what it does, the value it ships
  with and the range it accepts. Changes go straight into `edvr.ini`, and the
  game re-reads that file about once a second, so most of them take effect
  while you watch.
- Save logs, which puts the last session's logs, the breadcrumb file, any fatal
  note and your settings into one zip on your Desktop. That one file has
  everything [Reporting a problem](#reporting-a-problem) asks for, all from the
  right session.

Before it changes anything, it shows you exactly what it is about to do and
waits for a yes, and it copies every file it replaces into `edvr_backup\`
first. It asks for administrator rights only if the game is under `Program
Files`, and only when it reaches that step. It has no network access at all and
installs only what it carries. [docs/installer.md](docs/installer.md) explains
what it decides and why.

Because the installer is unsigned, Windows will say the program is
unrecognised; choose **More info → Run anyway**. Every release lists the
installer's SHA-256, and without a signature that hash is the only way to check
where the file came from.

To place the files yourself, follow
[docs/manual-install.md](docs/manual-install.md), which is the same install
done by hand. It shows where each file goes and covers the `openvr_api.dll`
rename that most manual installs get wrong.

### Checking it worked

Logs appear in `edvr_logs\` next to the game, and there are two of them. Once
the native OpenXR path is up, the second one, with `vr` in its name, says
`runtime,<name>,<version>`, naming the runtime it reached; an `error,` line in
its place means that startup step failed. **If there is no second log at all**,
the game is not on its OpenVR path and half the patch never loaded: see
[Headsets and VR runtimes](#headsets-and-vr-runtimes).

If you still see a flash, press **Pause** straight after it and send the logs.
Pause writes the last ten seconds of viewpoint history, which shows whether
EDVR detected the flash and let it through or never detected it at all.

### Reporting a problem

**Run `edvr-installer.exe` and press Save logs.** It writes one zip to your
Desktop with the last session's two logs, `edvr_breadcrumbs.txt`, any
`edvr_FATAL.txt` and your `edvr.ini`. That is everything listed below, all from
the right session. Then **open an issue** and attach the zip.

To do it by hand, attach `edvr_logs\` (both files if there are two). The log
records the build stamp, which fixes EDVR managed to install, and what each one
decided, so a report with the log attached can usually be diagnosed in one
pass; without it there is very little to go on. If the game will not start at
all, send `edvr_breadcrumbs.txt` from next to `EliteDangerous64.exe`. It is
written unbuffered, so it survives a crash that eats the log.

Report bugs in an issue, not on the [Discord](https://discord.gg/ynkdf6Gdua):
chat loses the attachments and the thread, and an issue keeps a problem on
record long enough to fix it.

### If the game dies a second or two after launch

**As of this version this fixes itself: update, and it should just work with no
`edvr.ini` change.** The launch crash
([#20](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/20)
and
[#21](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/21))
happened on rigs where Windows' `d3d11.dll` re-lays the render context's
dispatch table every frame. EDVR took a *frozen* copy of that table, the copy
fell out of step, and the GPU hung about a second and a half in. `auto` now
gives those rigs a *live* table that follows the runtime call by call, so the
default no longer crashes.

If `edvr_breadcrumbs.txt` ends at `arming d3d11 hooks`, the Direct3D half got
its hooks in and the game died shortly after. EDVR's crash sentinel then turns
those hooks off for the **next** launch by itself, so before this fix the usual
pattern was crash, play, crash, play. If a rig still dies that way after
updating, try these three settings under `[advanced]` in `edvr.ini`, in this
order:

```ini
[advanced]
context_hook_mode = shared
```

changes how EDVR attaches to the game's render context. By default, `auto`
checks whose code implements the context and picks for you: a *live* private
copy of the dispatch table (described under `live` below) when the methods are
Windows' own, and the shared table when another mod wraps them. The log line
says which it chose. `shared` forces the shared table. It is the most
conservative mode and composes with a wrapper such as ReShade. If anything
pushes EDVR out of a slot, the log says so by name.

```ini
[advanced]
context_hook_mode = live
```

is what `auto` already gives a rig whose render context is Windows' own, so
most machines run it without any setting. Set it by hand only to return to it
after trying `shared`. It gives the context a dispatch table of EDVR's own, so
nothing else in the process can write the table the game dispatches through,
and each entry reads the game's own entry at the moment of the call. Windows'
`d3d11.dll` re-lays the context's table while the game runs, sometimes onto a
different internal implementation, and a copy taken at startup does not follow
it: that frozen copy is the `private` mode, and it is what issues #20/#21 hung
on. The cost is two extra jumps per Direct3D call, too small to have shown up
in any frame time measured. The risk is that a mod wrapping Direct3D objects
(ReShade as `dxgi.dll`) does not expect its object to be re-pointed, which is
why `auto` gives those rigs `shared`. If `shared` keeps the game alive but the
log then says EDVR's hooks keep being pushed out of the table, `live` is the
mode that cannot be bypassed and never goes out of date. Each `live` hook
forwards through a small executable stub page that EDVR generates (mapped
`PAGE_EXECUTE_READ`: executable, but never also writable) to reach the
runtime's current method for that slot. An antivirus heuristic may weigh that
generated code, and a process that force-enables Control Flow Guard could
refuse a call through one.

```ini
[advanced]
d3d11_fixes = 0
```

turns the Direct3D fixes off for good. Nothing is hooked on the device or on
its render context, so the black void, the panel fixes, the shader replacements
and the anti-aliasing passes are all inert. The `openvr_api.dll` half keeps
working, and so do the swapchain and DXGI hooks that carry the frame boundary
it runs on, so EDVR is still active with this setting. A crash that survives it
is worth reporting, because it is then in one of those hooks or in the VR half.

Please report which of the three you needed, with the log from each, so the
workaround can be turned into a fix.

If you are willing to run one more session purely for the diagnosis, add

```ini
[advanced]
vtable_flip_timeline = 1
```

to whichever of the three you ended up on. It logs every change to the game's
Direct3D function table (what changed, from what to what, at which frame, and
which instruction did it) and writes the first few to `edvr_breadcrumbs.txt`,
which survives a crash that eats the log. That file shows whether the table
changed *before* the crash or *after* it, which none of the reports so far can
settle. Every line that carries a frame number now counts frames the same way,
including the monitor's "LONG FRAME" line, so you can read the order straight
off the file.

On `context_hook_mode = shared` the table changes every frame anyway, since
each change is Windows' own `d3d11.dll` writing its entry back over EDVR's
hook, so the per-change lines stop after the first few thousand and only the
running tally continues. That is expected. EDVR's own writes never appear in
the list, because it unlocks the memory before writing and so raises nothing
for the watch to see. The watch makes every write to the memory the table lives
on take an exception, which costs a few milliseconds a frame. It prints what it
cost and switches itself off if that ever gets serious, though never in the
first ten seconds, which is where the crash is. It is meant for one session:
set it back to 0 afterwards.

### VR failed to start after an EDVR update

EDVR is two files that must come from the same build: the graphics half
(`d3d11.dll`) and the VR half (`openvr_api.dll`). They agree on the size of a
message the VR half sends at startup, and a half-updated install (one file new,
the other old) fails that check on purpose so that neither runs at a size it
did not ask for. Elite then reports `VRInitError_Init_Internal`, and the native
log (`edvr_logs\edvr_openxr_*.log`) carries
`result,native_render_settings_query,-1` with no `openxr_render_size` lines
after it. That `-1` tells you one side is stale but not which.

Run `edvr-installer.exe` and press **Repair**. It writes both halves from the
one package it carries, so they cannot disagree. If you build from source, run
`python tools\install_edvr.py --target <store> --verify-only` (with `steam`,
`frontier`, or the path to the game directory) before every flight of a fresh
build. It compares each installed file's hash with the build and prints `native
verify mismatch:` with the path of the one that differs.

### Uninstall

Run `edvr-installer.exe` and press **Uninstall**. It removes EDVR's files and
renames the game's `openvr_api.dll` back. If another mod was chained behind
EDVR, it also puts that mod back under its own name, the step a manual
uninstall usually forgets. It leaves your `edvr.ini` alone unless you ask for
it to go, so a reinstall finds your settings again.

To uninstall by hand, delete `d3d11.dll` and `edvr.ini` from the game folder,
then, in whichever `Openvr` folder you used, delete EDVR's `openvr_api.dll` and
rename `openvr_api_orig.dll` back.

## What it fixes

Almost every one of them has the same shape: something correct on a monitor is
wrong in a headset, because it is drawn once for two eyes, pinned to your face
instead of standing in the world, or sized for a screen you are not looking at.

- The two eyes are made to agree. Without the fixes, one eye stops down near a
  star while the other does not (1.5 stops apart, measured; 0.4 with the fix),
  a planet renders as a black disc in one eye in the scanners, the FSS shows
  each eye a different scan, and the RemLok helmet's edge lines hang along your
  nose instead of at your temples.
- Things are put back in the world: a star's whole glare, which the stock game
  rolls and tilts with your head like a camera overlay; geyser plumes and solar
  prominences, which swim as you look past them; the loading ship's head-locked
  scan pattern; and the launch movie, moved off its 27-degree rectangle onto
  the splash screen's own surface.
- The one-frame flash each time you jump or drop out of supercruise is detected
  and not sent, so the runtime holds the previous frame.
- For shimmer and sharpness there is temporal anti-aliasing, with DLAA and DLSS
  on RTX cards. It automatically takes in UI and smoke depth and the exact
  motion of moving ships, vehicles and settlement parts from Elite's own
  records. AA is off by default; choose it, and the DLSS preset (default K),
  under Performance. RCAS sharpening is available separately.
- Over planets, Elite culls against a narrower frustum than it renders, so
  squares of ground at the edges of view go undrawn. The fix is off by default
  and costs about 6% GPU at the tested values.
- Busy settlements hold your frame rate down: at a crowded one Elite draws tens
  of thousands of small parts a frame, enough to hold the frame over the
  headset's refresh rate. `auto` (default) steps distant settlement detail down
  only while the frame runs long, gives it back the moment there is headroom,
  and returns to the game's own detail as soon as you leave; `reduced` keeps it
  down for as long as you are there. It works in the cockpit only for now.
  Measured at one settlement, the frame rate went from 45-50 fps to 70-80, with
  no visible change from the cockpit.
- On foot, the grey surround is made properly black, the screen is moved, bent
  and raised above its forced 1920x1080, and Explorer Cam gives you a real
  stereo view of your commander in the external camera.

[docs/fixes.md](docs/fixes.md) covers each fix in full: what it costs, what it
is measured at, and which setting turns it off. The defaults are what most
people want, and the exceptions are called out there and in the in-headset
menu.

## Explorer Cam

On foot, Elite renders the world once, flat, and shows that image to both eyes.
There is no depth because none is drawn. The external camera renders in proper
stereo, and that is where Explorer Cam works: while you are in that camera, it
moves your viewpoint to your commander's head. **It cannot make first person
3D** and does not try; the flat screen stays flat.

**It replaces one camera preset: Commander Right Shoulder.** On that preset the
camera sits at your commander's head in place of the preset's usual framing, so
cycle to it for the 3D view and away from it for normal framing. Every other
preset is untouched, and `advanced.head_offset_view` selects a different preset
to give up.

### It gives you no capability you do not already have

This matters more than the effect does. The external camera is Elite's own
feature, opened with your own binding, and inside it you cannot act: you cannot
shoot, scan, open a panel, use a terminal or pick anything up. To act you
switch back to first person, exactly as you do today. Explorer Cam changes
where the camera is while you are already in that mode and nothing else. It
gives no extra reach, reveals nothing the camera was not already showing, and
removes no step that anyone else has to take. A player with it and a player
without it can do the same things in the same order with the same clicks, and
only one of them sees it in 3D.

It touches nothing shared: no network path, no server state, nothing another
player observes, and no gameplay data read or written.

### Setting it up

1. You do not need to set any hotkeys. EDVR reads your external-camera and
   next-camera-view keys straight from your Elite key configuration, using the
   *on-foot* camera binding, which Elite keeps separate from the ship's. If
   they are on keyboard keys you are done, and the log's first lines name the
   keys it adopted and the file they came from. Rebind them in Elite, even
   mid-session, and EDVR follows within a few seconds. EDVR only *watches*
   these keys; it never presses them or interferes with the game receiving
   them.

   EDVR needs them because on screen, entering the camera looks identical to
   boarding your ship, and the camera key is how EDVR tells which it was. Near
   a planet the game also rebuilds its camera data every few seconds, and the
   next-view key's presses carry "which preset am I on" through the gaps.

   If your camera is bound **only to a controller**, bind a keyboard key for it
   in Elite (Options → Controls) for now. EDVR watches the keyboard, and
   controller support is planned.

2. Get on foot, open the camera, and cycle to **Commander Right Shoulder**, two
   presses from the view the camera opens on.

3. Tune the offsets with the headset on; they reload about once a second:

   ```
   head_offset_right   = -0.25   + is to your commander's right
   head_offset_up      = 0.25    + is up
   head_offset_forward = 1.25    + is the way your commander faces
   ```

   These are tuned for Commander Right Shoulder, which already sits close to
   your commander and faces the way they face, so the numbers are small, and
   the negative `right` brings you off the shoulder onto the centre line. Pick
   a preset several metres further back and `forward` becomes the large one,
   two to three metres instead of one. Treat them as starting points.

These offsets move the viewpoint of a headset you are wearing, so change them a
little at a time. Entering and leaving is a cut, not a glide, because the
game's own camera change is already a cut.

### What it does under the hood

Explorer Cam does two things the other fixes do not:

- It changes the headset position the game is told about. Each frame the game
  asks SteamVR where your head is, and EDVR adds your offset to the answer. The
  game then moves its *own* camera: as far as Elite knows, you leaned. Culling
  and object placement follow that camera, which is what makes it work.
- It reads one number from the game's memory: which external-camera view is
  showing, so the offset applies to the right preset. To find where that number
  lives, it searches once, on the first frame you are on foot, for a marker
  identifying the camera settings. It keeps nothing but the small view index,
  skips the game's code, and never writes.
- It reads event names from the game's journal, the documented file Elite
  writes in Saved Games for third-party tools, to know when gameplay has
  started, when you step onto your feet (where the game resets its camera
  view), and when a jump begins and resolves. It reads only the names
  (`LoadGame`, `Disembark`, `StartJump`, `FSDJump`, `SupercruiseEntry`) and
  reads or keeps no other content, and `d3d11.journal_watch = 0` turns it off
  entirely.

These safeguards are the reason to trust it:

- Nothing happens without your camera key, which EDVR only watches, as
  described above; it never presses or sends it.
- Your viewpoint moves at most 10 m per axis. Beyond that it clamps and says
  so, because refusing outright would snap the view, which is worse when you
  are wearing the headset.
- It counts your camera-key presses, and since build 332753 that is all it
  does. Reading the preset from the game is off by default (`camera_index_track
  = 0`). The read was a correction on top of the press count, and better where
  it worked, because it needs no key bound and cannot drift. But finding the
  records means walking every page the game holds, eleven to seventeen
  gigabytes, and a failed search retries four times. Build 332753 moved the
  marker, so on that build it read fifty to seventy gigabytes per session and
  found nothing. Turning it back on needs a marker measured on your own build;
  [docs/build-332753.md](docs/build-332753.md) has one for 332753 and shows how
  it was arrived at.
- It expires. The two halves of EDVR agree once a frame about which mode you
  are in. If the deciding half stops running, the half that moves your view
  stops trusting it within about a second and puts your viewpoint back.

Counting presses has a cost. The count is anchored to zero at launch and again
at every new on-foot session, which the game's own journal announces, so it is
right unless a press goes unseen. When one does, the count stays wrong until
you leave the camera and come back. With the read on, the next successful read
fixed it for you.

## The terrain fix (cull guard)

This is for Frontier issue
[72609](https://issues.frontierstore.net/issue-detail/72609), "Culling of
planet surface in VR too aggressive": the black squares at the edges of view
over planets. [docs/terrain-culling.md](docs/terrain-culling.md) covers what
was measured, why the fix works from outside the game, and what a fix inside it
would look like. It is off by default because it costs GPU time, and turning it
on takes three settings in `edvr.ini`.

1. Turn it on. Like every other cull-guard setting, this one is live:

   ```
   [fix]
   cull_guard = symmetric
   ```

2. Limit it to your headset (recommended). The `vr` log prints your headset's
   signature (`cull guard: this headset's signature is 94x99`); copy that value
   in:

   ```
   cull_guard_headsets = 94x99
   ```

   The guard then runs only on that headset, so on a rig that swaps headsets
   the other one pays nothing and you edit nothing when you swap.

3. Pick the margin. Left alone, the guard covers the full shortfall, which is
   guaranteed wherever the fix works at all and is the most expensive choice
   (~48% more rendered pixels on a Quest 3). The values tested on a Quest 3
   keep the edges clean at about 6%:

   ```
   cull_guard_fraction_h = 0.25
   cull_guard_fraction_v = 0
   ```

   Both are live, so save the file mid-flight and the guard picks them up. If
   black squares persist on your headset, raise `_h` in steps; the log's `cull
   guard margins` line names what each step leaves uncovered.

When the guard is working, the `vr` log says `cull guard stage 1`, then two
`cull guard LIVE` lines. `cull guard INERT` means the runtime shapes its
projections in a way the guard refuses to edit; the game runs normally, and
that log is worth attaching to an issue. The guard has been checked in the
field on Quest 3 via Virtual Desktop, where the missing tiles reproduced and
are now gone, and on Pimax via PiOpenXR. Real SteamVR is unmeasured so far, so
a log from there is a useful report whether the guard works or not.

## Settings

Everything is in `edvr.ini` next to the game, and if the file is missing you
get the defaults. `black_void`, `panel_distance` and the Explorer Cam offsets
reload while the game runs; the rest need a restart.

Press **F8** in the game (the key is `hotkey.menu`) to open the in-headset
menu: a settings panel appears where you are looking, anchored in the world so
it stays put while you read it. Up and Down pick a row, Left and Right change
it, Enter toggles, Tab changes page and Escape closes. The keys you already use
to walk Elite's own cockpit panels (up, down, left, right, select, back, next
and previous panel) work in the menu too, read from your Elite bindings
(`hotkey.read_game_bindings`), and the panel's bottom line names them. Tab, the
arrows, Enter and Escape always work as well. While the menu is open the game
sees no keyboard at all, so none of those keys reach the ship, though your
HOTAS and mouse still do. Every change is written to `edvr.ini` and applies the
way a hand edit would, and a row that only takes effect at the next launch says
so.

The Monitor page is fpsVR's readout (frame rate and 1% low, the app's and the
compositor's GPU time, dropped and reprojected frames, CPU, GPU, VRAM and RAM)
with a frame-time strip, and `menu.fps_overlay = on` pins a one-line version of
it to your view while the menu is closed. `menu.developer = on` adds the
advanced and experimental sections. The whole design is in
[docs/settings-menu.md](docs/settings-menu.md).

## Running alongside other mods

**The installer does all of this for you**: it recognises what is in the
`d3d11.dll` slot, renames it, and writes the setting. What follows is the same
procedure by hand, and it is also what the installer will tell you it did.

EDHM also installs as `d3d11.dll`. To run both:

1. Rename EDHM's `d3d11.dll` (say, to `d3d11_edhm.dll`) and leave it where it
   is.
2. Put EDVR's `d3d11.dll` in its place.
3. In `edvr.ini`, under `[advanced]`, set `real_dll = d3d11_edhm.dll`.

EDVR passes everything through EDHM, and anything EDHM doesn't handle falls
through to Windows' own `d3d11.dll`. Restart the game for this to take effect.
If the name is wrong or the file won't load, EDVR says so in the log and
carries on without it.

**EDHM's uninstaller runs `del d3d11.dll`**, which after this is *EDVR's* file.
To undo the pair cleanly, delete `d3d11.dll` and `edvr.ini`, rename
`d3d11_edhm.dll` back, then run EDHM's uninstaller if you want to. If the
uninstaller has already run, leaving EDVR gone and EDHM still parked under the
renamed file, the installer's Repair recognises that and puts both back.

ReShade needs no configuration. Install it the way ReShade tells you to
(normally as `dxgi.dll`) and EDVR composes with it, so both mods' effects
apply.

If something has gone wrong and the log has not answered it,
[docs/troubleshooting.md](docs/troubleshooting.md) covers the three faults with
a known cause and what each one needs: a launch crash on 0.7.1 or earlier,
every fix except the brightness one going quiet at once, and an old EDHM
pairing that crashed.

## Game updates

The brightness fix finds its target by what it does, not by which version of
Elite compiled it: it looks for the calculation that writes the exposure result
exactly twice per frame, and waits for that to hold for five frames running
before it acts. The black-void and screen-distance fixes key off image sizes
and a clear colour, and neither is version specific. The resolution fix looks
for the shape of the code it changes; its safeguards are described
[below](#what-it-does-and-does-not-do).

Two things are measured from a specific build (330683 / 4.4.0.3, the one this
was developed against), and both degrade instead of guessing. The 4.4.1.0
update (build 332753) moved the second of them and left the first alone;
[docs/build-332753.md](docs/build-332753.md) has the full re-check:

- The transition flash fix watches a viewpoint in a constant buffer. There is
  no instruction pattern to recognise, only a size and an offset, so it checks
  the *data*: a viewpoint moves smoothly, and EDVR requires the first 300
  rendered frames to behave that way before acting. If an update moves the
  block, whatever is at the old offset will not move like a viewpoint, and the
  fix disables itself and says so. It also switches off for the session if it
  ever withholds continuously, because permanent judder would be worse than the
  flash.
- Explorer Cam's camera marker will also move on update, and did in 332753.
  Reading the preset is now off by default and Explorer Cam counts key presses
  instead; its section above says what that costs.

The transition flash fix also recognises recurring false jumps and leaves them
alone. Flying low over terrain, the game alternates between shadow cameras
whose fixed separation looks like an enormous jump (measured at ~568,000 units,
recurring for eight minutes, each withhold felt as judder). A jump that keeps
recurring at the same size is a distance between render passes, not a
transition, because real transitions vary as real motion does. So the first
jump of a size is withheld and matching ones are left alone.
`transition_flash_repeat_percent` controls this, and the ini notes why raising
`transition_flash_units` cannot help: the false jumps are *larger* than real
ones. Both eyes of a frame follow one verdict, decided at whichever eye submits
first. When something changes, a `transition flash so far:` line counts
withheld and recognised jumps separately; if there is no such line, the fix
never fired.

Frontier's launcher may remove `d3d11.dll` when it verifies the install.
Nothing is broken when it does: it has simply uninstalled EDVR, so copy the
file back.

## What it does and does not do

EDVR loads alongside the game as a `d3d11.dll` proxy that forwards every call
to Windows' real `d3d11.dll`. Its `openvr_api.dll` is EDVR's own OpenXR
runtime, which implements the OpenVR interfaces Elite calls and speaks OpenXR
itself.

Most of the fixes never touch the game. They change how frames are drawn from
outside it: four small copies per frame so both eyes share an exposure value,
one substituted argument to a screen-clearing call, and one substituted copy of
the panel's position if you change the distance. For the transition flash, EDVR
reads a constant buffer the game has already filled and, on the rare frame
drawn from the wrong place, does not submit that frame to the OpenXR runtime.
That read is camera state, not gameplay state, and EDVR never writes to the
buffer it reads. The only action it can take is to not submit a frame, or to
hand the runtime the game's own previous frame in its place: a copy EDVR keeps
of the last frame it submitted, which is always the game's content and never
EDVR's.

The temporal pass and the sharpening (`temporal_aa`, `render_sharpness`, both
off by default) each run one GPU pass over the game's finished frame into a
texture EDVR owns, and the runtime receives that copy. The game's texture is
read and never written, and no answer the game asks for changes. The temporal
pass also shifts the projection the game is told by a fraction of a pixel each
frame, the way the terrain fix shifts it by a margin.

For moving objects the temporal pass goes further, and only while `temporal_aa`
is on. It hooks Elite's own functions that update and pack moving ships,
vehicles and settlement parts each frame. The hooks live in memory only, and
only in the executable they were verified against; on anything else the pass
stands down and says so in the log. It reads each object's position and
orientation from the game's render records and writes last frame's into an
unused part of the same record, which the game then hands the GPU. It also
swaps the shaders those objects are drawn with for copies that write one extra
output, recording which record drew each pixel and at what depth, into a target
EDVR owns. What they draw into the game's own targets is unchanged, bit for
bit.

Other fixes do more too, and each is described in full. The resolution fix
(below) rewrites twelve numbers in the game's code. Explorer Cam
([above](#explorer-cam)) reads one number from the game's memory and changes
the headset position the game is told about. The cull guard
([above](#the-terrain-fix-cull-guard)) changes the field of view the game is
told the headset shows; the game then draws the wider view itself, and EDVR
submits only the true region, copied from the game's own frame. The cull guard
edits answers, never memory, so the runtime and anything else that asks always
receive the truth, and before changing anything it validates the runtime's
projection against the shape it expects, standing down loudly on a mismatch.
None of them does anything until you configure it.

The resolution fix, `auto` by default, rewrites the twelve numbers that are the
width and height the game forces for the on-foot screen, in the places it
forces them. It changes nothing else, neither the surrounding instructions nor
the game's decision about which screen to draw. `auto` sizes it from what your
headset actually rendered per eye last session; a fresh install runs it as
"off", the same stock 1920x1080 as before, until one session with VR running
has completed. Typing a width in pixels overrides it, and the height always
follows at 16:9. These safeguards are the reason to trust it:

- No file on disk is modified. The change exists only in memory, and the
  original values are put back when the game closes.
- It recognises the code it changes rather than trusting a version number. The
  shape is very specific: a check on which screen is being drawn, a forced 16:9
  size only in that case, and the real size read from the game's data
  otherwise. On the build this was developed against, that shape occurred in
  six places in 81 MB of code. EDVR accepts between three and twelve, provided
  they all agree, so an update that adds or drops one does not switch the
  feature off.
- If what it finds looks wrong (too few, too many, or places that disagree), it
  refuses, does nothing, and says so in the log.
- It cannot corrupt the game's code. It replaces a number with another number
  of the same size, so instruction lengths and program flow are untouched. If
  it ever matched the wrong thing, the worst case is something drawn at an odd
  size, which you would see and which is gone on restart.
- It logs what it found before changing it, so the log shows the resolution the
  game was really forcing.
- It is all or nothing: if any single write fails, every earlier one is undone.

No fix here touches the network, your account, or anything the server sees, and
none reads or changes gameplay state (position, ship, cargo, credits,
missions). None interacts with anti-cheat, and none attempts to hide from
anything.

If you would rather no part of this went near the game's code or memory, set
`vscreen_res_width` to `1920` (the stock size, meaning "do not patch"), leave
Explorer Cam unconfigured and leave `temporal_aa` off. The DLL then behaves as
earlier versions did.

## Build

Building needs Visual Studio 2022 with the C++ workload, and Python. First
fetch the pinned official Khronos loader and its notice:

```
python tools\fetch_openxr_loader.py
```

Then build:

```
build.bat
```

That produces the native graphics/runtime pair, `build\openxr_loader.dll`, and
`build\smoke.exe`; the last of these checks the build without the game or a
headset:

```
build\smoke.exe build\d3d11.dll
```

The optional DLSS mode, and an installer that carries NVIDIA's runtime, need
the DLSS SDK on the machine. Its licence keeps it out of this repository, and
it is not in the graphics driver either. This command

```
python tools\fetch_ngx.py
```

fetches one pinned commit of NVIDIA's public SDK repository into
`%LOCALAPPDATA%\EDVR\ngx-sdk` and checks the runtime's hash against the pin in
the script. `EDVR_NGX_SDK` points the build at a copy somewhere else.

Generating the installer's resources needs the native pair and the bundled
loader; there is no legacy OpenVR-only package. `package.bat <version>
--no-dlss` packages a build made without the DLSS SDK. When the build contains
DLSS, the archive keeps its matching DLL and NVIDIA license notice next to the
installer that embeds it.

## Antivirus

A DLL that sits next to a game executable and intercepts graphics calls looks,
structurally, like something worth flagging, and some scanners will flag it.
The source is here so you can read exactly what it does and build it yourself.

## Licence and standing

EDVR is MIT licensed; see [LICENSE](LICENSE). One file the installer carries is
not: NVIDIA's DLSS runtime, `nvngx_dlss.dll`, is NVIDIA's software under the
NVIDIA RTX SDKs licence. It is distributed unmodified as part of this
application, as that licence allows, and only placed on machines with an NVIDIA
card. The `fsr` engine is AMD's FidelityFX Super Resolution 3.1 upscaler
through its community Direct3D 11 port (the optiscaler project's
FidelityFX-SDK-DX11, MIT), compiled into `d3d11.dll`; its notice ships as
`FIDELITYFX-SDK-DX11-LICENSE.txt`.

Not affiliated with, endorsed by, or supported by Frontier Developments plc or
Valve Corporation. Elite Dangerous is a trademark of Frontier Developments plc.
