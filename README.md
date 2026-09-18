# EDVR — an unofficial patch for Elite Dangerous: Odyssey in VR

Fixes for things that make Odyssey uncomfortable in a headset. Two dozen fixes,
a native OpenXR package, about three minutes — the short list is under [What it
fixes](#what-it-fixes), and each one in full is in
[docs/fixes.md](docs/fixes.md).

**The Windows release uses Elite's OpenVR-facing path to reach native OpenXR.**
The bundled Khronos loader uses the Windows Active OpenXR Runtime. SteamVR is a
valid selected runtime; no SteamVR loader or OpenComposite installation is
required. Elite's legacy LibOVR path is not a fallback.

**Something not working?** [Open an
issue](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/new/choose)
— that is the place a bug gets fixed, because you can attach the log, and the
log is usually the whole answer. For everything else — setup questions, "is
this normal", or just talking about it — there is a
[Discord](https://discord.gg/ynkdf6Gdua).

EDVR is free and stays free. If it improves your VR experience, [tips are
welcome](https://ko-fi.com/seancharacterecho) — please do not feel any
obligation to do so.

> **Already running EDHM or ReShade?** Both can run alongside EDVR. EDHM
> installs itself as `d3d11.dll` too, and only one file can have that name —
> don't overwrite it. The installer handles this for you; by hand it is one
> rename and one setting, under [Running alongside other
> mods](#running-alongside-other-mods). ReShade needs nothing at all as of
> 0.7.2.

## Headsets and VR runtimes

**EDVR supports the native OpenXR route on Windows.** The selected Active
Runtime may be SteamVR. Pimax over PiOpenXR, Quest 3 over Virtual Desktop's
VDXR, and the latest Air Link flight are the current field checks; other
headsets and runtimes are not implied to be qualified.

Elite's OpenVR-facing entry point is retained for compatibility, while EDVR's
native runtime facade owns the OpenXR session. The package has no backend
choice and does not silently fall back to LibOVR or a legacy EDVR pair.

The bundled loader can select SteamVR, PiOpenXR, VDXR, or Meta's OpenXR
runtime. A compatible runtime and connected headset must be available before
launch. These choices use the same native EDVR backend.

The legacy LibOVR path is unsupported. A release installer validates the Elite
executable profile and refuses an unsupported revision before writing, rather
than silently selecting a legacy route.

The Windows OpenXR Active Runtime selector matters to the bundled loader. Set
it to the runtime you intend to use before launching Elite.

**Which runtime am I on?** Look in `edvr_logs\` next to the game after a
session:

| What you find | What it means |
|---|---|
| Native OpenXR startup and runtime name | The bundled loader reached the Windows Active Runtime. |
| Unsupported profile error | This game revision needs an updated EDVR profile. |
| No native startup log | Native startup has not been confirmed; attach the available logs for diagnosis. |

The native route is selected by the Windows Active Runtime. Do not install
OpenComposite or a vendor OpenXR loader to make the package work. User launches
Elite normally after choosing the desired Windows runtime.

## Install

**Run `edvr-installer.exe`** from the release. One file, nothing to extract,
nothing to put in the right folder — it carries the native graphics/runtime
pair, bundled OpenXR loader and notice, `edvr.ini`, and optional DLSS runtime:

- **Finds the game.** Frontier launcher, Steam or Epic, on whichever drive — it
  asks each launcher where it put the game rather than guessing paths, and
  confirms every answer by finding `EliteDangerous64.exe`. Or point it at a
  folder yourself.
- **Places the bundled OpenXR loader** beside the native runtime and selects
  Windows' Active Runtime. It does not search for or load a SteamVR or vendor
  loader.
- **Keeps your settings.** Updating never overwrites `edvr.ini`: it writes the
  new version's file with your values put back into it, so new settings and
  changed defaults arrive and nothing you tuned is lost. It says which is which
  afterwards.
- **Preserves the game's original `openvr_api.dll`** as a recovery backup for
  uninstall; it never treats a missing original as permission to fall back.
- **Leaves other mods working.** If EDHM (or anything else) is already
  installed as `d3d11.dll`, it renames that aside, takes the name, and sets
  `advanced.real_dll` so EDVR passes every call through to it.
- **Repair**, for when another mod's installer overwrites EDVR's files — a
  common one is EDHM's uninstaller running `del d3d11.dll`, which after an EDVR
  install is *ours*. Repair puts both back, side by side.
- **Uninstall**, which renames back everything it renamed.
- **A settings screen.** Every setting, with what it does, the value it ships
  with and the range it accepts, instead of a text editor. Changes go straight
  into `edvr.ini` — the game re-reads that file about once a second, so most of
  them are live while you watch.
- **Save logs**, which puts the last session's logs, the breadcrumb file, any
  fatal note and your settings into one zip on your Desktop — everything
  [Reporting a problem](#reporting-a-problem) asks for, from the right session,
  in one file to attach.

It shows you exactly what it is about to do and waits for a yes, and every file
it replaces is copied into `edvr_backup\` first. It needs administrator rights
only if the game is under `Program Files`, and asks at that point rather than
up front. It has no network access at all — it installs what it carries. *What
it decides and why: [docs/installer.md](docs/installer.md).*

The installer and both DLLs are signed from the release workflow: Windows
should show the publisher as SignPath Foundation, which signs open-source
projects' builds made on public CI. If Windows still calls a fresh release
unrecognised, **More info → Run anyway**. Every release still lists each
file's SHA-256, the only check available for a download.

**Would rather place the package yourself?**
[docs/manual-install.md](docs/manual-install.md) is the same install by hand:
where each file goes, and the `openvr_api.dll` rename that most manual installs
get wrong.

### Checking it worked

Logs appear in `edvr_logs\` next to the game. There are two of them; the
second, with `vr` in the name, says `runtime,<name>,<version>` once the native
OpenXR path is up, naming the runtime it reached. An `error,` line there
instead means that startup step failed. **If there is no second log at all**,
the game is not on its OpenVR path and half the patch never loaded — see
[Headsets and VR runtimes](#headsets-and-vr-runtimes). If you see a flash
anyway, press **Pause** straight after and send the logs — that writes the last
ten seconds of viewpoint history, which separates "detected and let through"
from "never detected".

### Reporting a problem

**Run `edvr-installer.exe` and press Save logs.** It writes one zip to your
Desktop with the last session's two logs, `edvr_breadcrumbs.txt`, any
`edvr_FATAL.txt` and your `edvr.ini` — everything below, from the right
session, in one file to attach. Then **open an issue** with it.

By hand: attach `edvr_logs\` — both files if there are two. The log carries the
build stamp, which fixes it were able to install, and what each one decided, so
a report with it attached is usually diagnosable in one pass. Without it there
is very little to go on. If the game will not start at all,
`edvr_breadcrumbs.txt` next to `EliteDangerous64.exe` is written unbuffered and
survives a crash that eats the log — send that.

The [Discord](https://discord.gg/ynkdf6Gdua) is good for setup questions and
for "is this normal". Bugs still want an issue: chat loses attachments and the
thread, and an issue is what remembers a problem long enough to fix it.

### If the game dies a second or two after launch

**As of this version this fixes itself — update and it should just work, with
no `edvr.ini` change.** The launch crash
([#20](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/20)
and
[#21](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/21))
was EDVR taking a *frozen* copy of the render context's dispatch table on rigs
where Windows' `d3d11.dll` re-lays that table every frame; the copy fell out of
step and hung the GPU about a second and a half in. `auto` now gives those rigs
a *live* table instead — one that follows the runtime call by call — so the
default no longer crashes. If you were on a crashing build, the fix is to
update, nothing more.

`edvr_breadcrumbs.txt` ending at `arming d3d11 hooks` means the Direct3D half
got its hooks in and the game died shortly after. EDVR's crash sentinel turns
those hooks off for the **next** launch on its own, so before this fix the
usual shape was crash, play, crash, play. If a rig somehow still dies that way
after updating, three settings under `[advanced]` in `edvr.ini` are worth
trying, in this order:

```ini
[advanced]
context_hook_mode = shared
```

changes how EDVR attaches to the game's render context. `auto` (the default)
asks whose code implements the context and picks for you: a *live* private copy
of the dispatch table (described under `live` below) when the methods are
Windows' own, the shared table when another mod wraps them. The log line says
which it chose. `shared` forces the shared table — the most conservative mode,
the one that composes with a wrapper like ReShade, and the one to reach for
first if a rig somehow still crashes after updating. If anything pushes EDVR
out of a slot the log says so by name.

```ini
[advanced]
context_hook_mode = live
```

is what `auto` already gives a rig whose render context is Windows' own, so on
most machines you are running it without setting anything — name it by hand
only to come back to it after trying `shared`. It gives the context a dispatch
table of EDVR's own — so nothing else in the process can write the table the
game dispatches through — and every entry in it reads the game's own entry at
the moment of each call instead of remembering what it said at startup. That
matters because Windows' `d3d11.dll` re-lays the context's table while the game
runs, sometimes onto a different internal implementation, and a copy taken at
startup does not follow it (that frozen copy is the `private` mode, and it is
what issues #20/#21 hung on). `live` follows it, call by call. The cost is two
extra jumps per Direct3D call, which is below anything that has been measurable
in a frame time; the risk is that a mod wrapping Direct3D objects (ReShade as
`dxgi.dll`) does not expect its object to be re-pointed, which is why `auto`
gives those rigs `shared` instead. If `shared` keeps the game alive but the log
then says EDVR's hooks keep being pushed out of the table, this is the mode
that is both unbypassable and never out of date. Each `live` hook forwards
through a small executable stub page EDVR generates -- mapped
`PAGE_EXECUTE_READ`, executable but never also writable -- so it can reach the
runtime's current method for that slot; that generated code is something an
antivirus heuristic may weigh, and a process that force-enables Control Flow
Guard could refuse a call through one.

```ini
[advanced]
d3d11_fixes = 0
```

turns the Direct3D fixes off for good. Nothing is hooked on the device or on
its render context, so the black void, the panel fixes, the shader replacements
and the anti-aliasing passes are all inert. The `openvr_api.dll` half keeps
working, and so do the swapchain and DXGI hooks that carry the frame boundary
it runs on -- so this is not "EDVR loads and does nothing", and if a crash
survives it, that is worth reporting: it is in one of those or in the VR half.

Please report which of the three you needed, with the log from each -- that is
the measurement that turns a workaround into a fix.

If you are willing to run one more session for the diagnosis rather than for
your own comfort, add

```ini
[advanced]
vtable_flip_timeline = 1
```

to whichever of the three you ended up on. It logs every change to the game's
Direct3D function table -- what changed, from what to what, at which frame, and
which instruction did it -- and writes the first few to `edvr_breadcrumbs.txt`,
which survives a crash that eats the log. That file is what says whether the
table changed *before* the crash or *after* it, which is the one thing the
reports so far cannot settle. Every line that carries a frame number now counts
frames the same way, including the monitor's "LONG FRAME" line, so the ordering
can be read straight off the file.

On `context_hook_mode = shared` the table changes every frame anyway — that is
Windows' own `d3d11.dll` writing its entry back over EDVR's hook — so the
per-change lines stop after the first few thousand and only the running tally
continues. That is expected, not a fault. (EDVR's own writes never appear in
the list: it unlocks the memory before writing, so they raise nothing for the
watch to see.) It makes every write to the memory the table lives on take an
exception, which costs a few milliseconds a frame; it prints what it cost,
switches itself off if that ever gets serious (never in the first ten seconds,
which is where the crash is), and is meant for one session. Set it back to 0
afterwards.

### VR failed to start after an EDVR update

EDVR is two files that must be from the same build: the graphics half
(`d3d11.dll`) and the VR half (`openvr_api.dll`). They agree on the size of a
message the VR half sends at startup, and a half-updated install — one file
new, the other old — fails that check on purpose rather than running at a size
it did not ask for. Elite then reports `VRInitError_Init_Internal` and the
native log (`edvr_logs\edvr_openxr_*.log`) carries
`result,native_render_settings_query,-1` with no `openxr_render_size` lines
after it. That `-1` says one side is stale, not which.

Run `edvr-installer.exe` and press **Repair**: it writes both halves from the
one package it carries, so they cannot disagree. If you build from source,
`python tools\install_edvr.py --target <store> --verify-only` (`steam`,
`frontier`, or the path to the game directory) compares each installed file's
hash with the build and prints `native verify mismatch:` with the path of the
one that differs; run it before every flight of a fresh build.

### Uninstall

Run `edvr-installer.exe` and press **Uninstall**. It removes EDVR's files,
renames the game's `openvr_api.dll` back, and — if another mod was chained
behind EDVR — puts that mod back under its own name, which is the step a manual
uninstall usually forgets. Your `edvr.ini` is left alone unless you ask for it
to go, so reinstalling finds your settings again.

By hand: delete `d3d11.dll` and `edvr.ini` from the game folder; in whichever
`Openvr` folder you used, delete EDVR's `openvr_api.dll` and rename
`openvr_api_orig.dll` back.

## What it fixes

Almost every one of them is the same shape: something correct on a monitor is
wrong in a headset — drawn once for two eyes, pinned to your face instead of
standing in the world, or sized for a screen you are not looking at.

- **The two eyes made to agree.** One eye stopping down near a star while the
  other does not (1.5 stops apart, measured; 0.4 with the fix). The planet that
  renders as a black disc in one eye in the scanners. The FSS showing each eye
  a different scan. The RemLok helmet's edge lines hanging along your nose
  instead of at your temples.
- **Things put back in the world.** A star's whole glare, which stock rolls and
  tilts with your head like a camera overlay. Geyser plumes and solar
  prominences, which swim as you look past them. The loading ship's head-locked
  scan pattern. The launch movie, off its 27-degree rectangle and onto the
  splash screen's own surface.
- **The one-frame flash** each time you jump or drop out of supercruise —
  detected and not sent, so the runtime holds the previous frame instead.
- **Shimmer and sharpness.** Temporal anti-aliasing, with DLAA and DLSS on RTX
  cards, includes UI and smoke depth and rotating-station motion automatically.
  Choose it and the **DLSS preset** (default K) on Performance; AA remains off
  by default. RCAS sharpening is available separately.
- **The terrain missing at the edges of view** over planets — Elite culls
  against a narrower frustum than it renders, so squares of ground go undrawn.
  Off by default; it costs about 6% GPU at the tested values.
- **On foot:** the grey surround made properly black, the screen moved, bent
  and raised above its forced 1920x1080, and Explorer Cam, which gives you a
  real stereo view of your commander in the external camera.

**Each fix in full — what it costs, what it is measured at, and which setting
turns it off — is in [docs/fixes.md](docs/fixes.md).** The defaults are what
most people want; the exceptions are called out there and in the in-headset
menu.

## Explorer Cam

On foot, Elite renders the world once, flat, and shows that image to both eyes
— there is no depth because none is being drawn. The external camera renders in
proper stereo, so that is where Explorer Cam works: it moves your viewpoint to
your commander's head while you are in that camera. **It cannot make first
person 3D** — the flat screen stays flat — and it does not try.

**It replaces one camera preset: Commander Right Shoulder.** On that preset the
camera sits at your commander's head instead of the preset's usual framing;
cycle to it for the 3D view, off it for normal framing. Every other preset is
untouched. `advanced.head_offset_view` selects a different preset to give up
instead.

### It gives you no capability you do not already have

This matters more than the effect does. The external camera is Elite's own
feature, opened with your own binding, and inside it you cannot act: no
shooting, scanning, opening a panel, using a terminal, or picking anything up.
To act you switch back to first person, exactly as you do today. Explorer Cam
changes **where the camera is while you are already in that mode** and nothing
else — no extra reach, nothing revealed the camera was not already showing, no
step removed that anyone else has to take. A player with it and a player
without it can do the same things, in the same order, with the same clicks. One
of them is looking at it in 3D.

It touches nothing shared: no network path, no server state, nothing another
player observes, and no gameplay data read or written.

### Setting it up

1. **Hotkeys: nothing to do.** EDVR reads your external-camera and
   next-camera-view keys straight from your Elite key configuration — the
   *on-foot* camera binding, which Elite keeps separate from the ship's. If
   they are on keyboard keys, you are done: the log's first lines name the keys
   it adopted and the file they came from. Rebind them in Elite, even
   mid-session, and EDVR follows within a few seconds. EDVR only *watches*
   these keys; it never presses them or interferes with the game receiving
   them.

   They matter because on screen, entering the camera looks identical to
   boarding your ship — the camera key is how EDVR knows which it was. And near
   a planet the game rebuilds its camera data every few seconds, so the
   next-view key's presses are what carry "which preset am I on" through the
   gaps.

   If your camera is bound **only to a controller**, bind a keyboard key for it
   in Elite (Options → Controls) for now — EDVR watches the keyboard, and
   controller support is planned.

2. Get on foot, open the camera, and cycle to **Commander Right Shoulder** —
   two presses from the view the camera opens on. That is the preset the offset
   replaces; every other preset keeps its normal framing.

3. Tune the offsets with the headset on; they reload about once a second:

   ```
   head_offset_right   = -0.25   + is to your commander's right
   head_offset_up      = 0.25    + is up
   head_offset_forward = 1.25    + is the way your commander faces
   ```

   These are tuned for Commander Right Shoulder, which already sits close to
   your commander and faces the way they face — so the numbers are small, and
   the negative `right` brings you off the shoulder onto the centre line. Pick
   a preset several metres further back and `forward` becomes the large one,
   two to three metres instead of one. Starting points, not universal answers.

**Comfort.** These move the viewpoint of a headset you are wearing; change them
a little at a time. Entering and leaving is a cut rather than a glide, because
the game's own camera change is already a cut.

### What it does under the hood

Two things the other fixes do not:

- **It changes the headset position the game is told about.** Each frame the
  game asks SteamVR where your head is; EDVR adds your offset to the answer.
  The game then moves its *own* camera — as far as Elite knows, you leaned.
  That is what makes it work: culling and object placement follow.
- **It reads one number from the game's memory:** which external-camera view is
  showing, so the offset applies to the right preset. To find where that number
  lives it searches once, on the first frame you are on foot, for a marker
  identifying the camera settings — it keeps nothing but the small view index,
  skips the game's code, and never writes.
- **It reads event names from the game's journal** — the documented file Elite
  writes for third-party tools in Saved Games — to know when gameplay has
  started, when you step onto your feet (where the game resets its camera
  view), and when a jump begins and resolves. Names only (`LoadGame`,
  `Disembark`, `StartJump`, `FSDJump`, `SupercruiseEntry`); no other content is
  read or kept, and `d3d11.journal_watch = 0` turns it off entirely.

Its safeguards, because they are the reason to trust it:

- **Nothing happens without your camera key**, as above — and EDVR only
  *watches* that key; it never presses or sends it.
- **Your viewpoint moves at most 10 m per axis.** Beyond that it clamps and
  says so — refusing outright would snap the view, which is worse when worn.
- **It counts your camera-key presses**, and since build 332753 that is all it
  does. Reading the preset out of the game is off by default — see below.
- **It expires.** The two halves of EDVR agree once a frame about which mode
  you are in; if the deciding half stops running, the half that moves your view
  stops trusting it within about a second and puts your viewpoint back.
- **Reading the preset from the game is off** (`camera_index_track = 0`). It
  was a correction on top of the press count: better where it worked, because
  it needs no key bound and cannot drift. Finding the records means walking
  every page the game holds, which is eleven to seventeen gigabytes, and a
  failed search retries four times. Build 332753 moved the marker, so on that
  build it was fifty to seventy gigabytes of reading per session that found
  nothing. Turning it back on needs a marker measured on your own build;
  [docs/build-332753.md](docs/build-332753.md) has one for 332753 and shows how
  it was arrived at.
- **What that costs you.** The press count is anchored to zero at launch and to
  zero again at every new on-foot session, which the game's own journal
  announces, so it is right unless a press goes unseen. When one does, the
  count stays wrong until you leave the camera and come back. With the read on,
  the next successful read fixed it for you.

## The terrain fix (cull guard)

For Frontier issue [72609](https://issues.frontierstore.net/issue-detail/72609)
— "Culling of planet surface in VR too aggressive", the black squares at the
edges of view over planets. What was measured, why the fix works from outside
the game, and what a fix inside it would look like:
[docs/terrain-culling.md](docs/terrain-culling.md). It is off by default
because it costs GPU time; enabling it is three settings in `edvr.ini`.

1. **Turn it on** — live, like every other cull-guard setting:

   ```
   [fix]
   cull_guard = symmetric
   ```

2. **Gate it to your headset** (recommended). The `vr` log prints your
   headset's signature — `cull guard: this headset's signature is 94x99` — copy
   that value in:

   ```
   cull_guard_headsets = 94x99
   ```

   The guard then runs only on that headset. On a rig that swaps headsets, the
   other one pays nothing, with no ini edits at swap time.

3. **Pick the margin.** Left alone the guard covers the full shortfall —
   guaranteed wherever the fix works at all, and the most expensive (~48% more
   rendered pixels on a Quest 3). The values tested on a Quest 3 keep the edges
   clean at about **6%**:

   ```
   cull_guard_fraction_h = 0.25
   cull_guard_fraction_v = 0
   ```

   Both are live — save the file mid-flight and the guard picks them up. If
   black squares persist on your headset, raise `_h` in steps; the log's `cull
   guard margins` line names what each step leaves uncovered.

Working, the `vr` log says `cull guard stage 1`, then two `cull guard LIVE`
lines. `cull guard INERT` means this runtime shapes its projections in a way
the guard refuses to edit — the game runs normally, and that log is worth
attaching to an issue. Field-verified on Quest 3 via Virtual Desktop (where the
missing tiles reproduced, and are gone) and Pimax via PiOpenXR; real SteamVR is
unmeasured so far, so a log from there is a useful report either way.

## Settings

Everything is in `edvr.ini`, next to the game; with the file missing you get
the defaults. `black_void`, `panel_distance` and the Explorer Cam offsets
reload while the game runs; the rest need a restart.

**The in-headset menu.** Press **F8** in the game (the key is `hotkey.menu`)
and a settings panel appears where you are looking, anchored in the world so it
stays put while you read it. Up and Down pick a row; Left and Right change it;
Enter toggles; Tab changes page; Escape closes. The keys you already use to
walk Elite's own cockpit panels -- up, down, left, right, select, back, next
and previous panel -- work in the menu too, read from your Elite bindings
(`hotkey.read_game_bindings`), and the panel's bottom line names them. Tab, the
arrows, Enter and Escape always work as well. While it is open the game sees no
keyboard at all, so none of those keys reach the ship -- your HOTAS and mouse
still do. Every change is written to `edvr.ini` and applies the way a hand edit
would, and a row that only takes effect at the next launch says so. The
**Monitor** page is fpsVR's readout -- frame rate and 1% low, the app's and the
compositor's GPU time, dropped and reprojected frames, CPU, GPU, VRAM and RAM
-- with a frame-time strip; `menu.fps_overlay = on` pins a one-line version of
it to your view while the menu is closed. `menu.developer = on` adds the
advanced and experimental sections. The whole design is in
[docs/settings-menu.md](docs/settings-menu.md).

## Running alongside other mods

**The installer does all of this for you** — it recognises what is in the
`d3d11.dll` slot, renames it, and writes the setting. What follows is the same
procedure by hand, and what the installer will tell you it did.

**EDHM** also installs as `d3d11.dll`. To run both:

1. **Rename** EDHM's `d3d11.dll` — say `d3d11_edhm.dll` — leaving it in place.
2. **Put EDVR's `d3d11.dll`** in its place.
3. In `edvr.ini`, under `[advanced]`, set `real_dll = d3d11_edhm.dll`.

EDVR passes everything through it; anything it doesn't handle falls through to
Windows' own `d3d11.dll`. Restart to take effect. If the name is wrong or the
file won't load, EDVR says so in the log and carries on without it.

**One catch with EDHM:** its uninstaller runs `del d3d11.dll`, which after this
is *EDVR's* file. To undo the pair cleanly: delete `d3d11.dll` and `edvr.ini`,
rename `d3d11_edhm.dll` back, then run EDHM's uninstaller if wanted. (The
installer's **Repair** recognises the aftermath of this — EDVR gone, EDHM still
parked under the renamed file — and puts both back.)

**ReShade** needs no configuration — install it the way ReShade tells you to
(normally as `dxgi.dll`) and EDVR composes with it. Both mods' effects apply.

**If something has gone wrong** and the log has not answered it — a launch
crash on 0.7.1 or earlier, every fix except the brightness one going quiet at
once, or an old EDHM pairing that crashed —
[docs/troubleshooting.md](docs/troubleshooting.md) has the three faults with a
known cause and what each one needs.

## Game updates

**The brightness fix finds its target by what it does**, not by which version
of Elite compiled it: the calculation that writes the exposure result exactly
twice per frame, stable for five frames running, before it acts. The black-void
and screen-distance fixes key off image sizes and a clear colour, neither
version specific. **The resolution fix** looks for the shape of the code it
changes rather than trusting a version number — its safeguards are described
[below](#what-it-does-and-does-not-do).

**Two things are measured from a specific build** (330683 / 4.4.0.3, the one
this was developed against), and both degrade rather than guess. The 4.4.1.0
update (build 332753) moved the second of them and left the first alone —
[docs/build-332753.md](docs/build-332753.md) has the full re-check:

- **The transition flash fix** watches a viewpoint in a constant buffer — no
  instruction pattern to recognise, only a size and an offset. So it checks the
  *data*: a viewpoint moves smoothly, and EDVR requires the first 300 rendered
  frames to behave that way before acting. If an update moves the block, what
  is at the old offset will not move like a viewpoint, and the fix disables
  itself and says so. It also switches off for the session if it ever withholds
  continuously — permanent judder would be worse than the flash.
- **Explorer Cam's camera marker** will also move on update — and did, in
  332753. Reading the preset is now off by default and Explorer Cam counts key
  presses instead; see its section above for what that costs.

**Recurring false jumps are recognised and left alone.** Flying low over
terrain, the game alternates between shadow cameras whose fixed separation
looks like an enormous jump — measured at ~568,000 units, recurring for eight
minutes, each withhold felt as judder. A jump that keeps recurring at the same
size is a distance between render passes, not a transition — real transitions
vary, because real motion does — so the first of a size is withheld and
matching ones are left alone. `transition_flash_repeat_percent` controls it;
the ini notes why raising `transition_flash_units` cannot help (the false jumps
are *larger* than real ones, not smaller). Both eyes of a frame also follow one
verdict, decided at whichever eye submits first. A `transition flash so far:`
line counts withheld and recognised separately when something changes; no such
line means it never fired.

Frontier's launcher may remove `d3d11.dll` when it verifies the install — that
is not a fault, it has simply uninstalled EDVR. Copy the file back.

## What it does and does not do

It loads alongside the game as a `d3d11.dll` proxy, forwarding every call to
Windows' real `d3d11.dll`; `openvr_api.dll` is EDVR's own OpenXR runtime,
implementing the OpenVR interfaces Elite calls and speaking OpenXR itself.

**Most of the fixes never touch the game.** They change how frames are drawn
from outside it: four small copies per frame so both eyes share an exposure
value, one substituted argument to a screen-clearing call, one substituted copy
of the panel's position if you change the distance, and — for the transition
flash — reading a constant buffer the game has already filled and, on the rare
frame drawn from the wrong place, not submitting that frame to the OpenXR
runtime. That read is camera state, not gameplay state; it never writes to the
buffer it reads, and the only action it can take is to not submit a frame — or
to hand the runtime the game's own previous frame in its place: a copy EDVR
keeps of the last frame it submitted, always the game's content, never EDVR's.

**The temporal pass and the sharpening** (`temporal_aa`, `render_sharpness`,
both off by default) are each one GPU pass over the game's finished frame,
into a texture EDVR owns, and that copy is what the runtime receives —
the game's texture is read, never written, no answer the game asks for
changes, and nothing is read from memory. The temporal pass also shifts
the projection the game is told by a fraction of a pixel each frame,
the way the terrain fix shifts it by a margin.

**Three fixes do more, and each is described in full:** the resolution fix
(below) rewrites twelve numbers in the game's code; Explorer Cam
([above](#explorer-cam)) reads one number from the game's memory and changes
the headset position the game is told about; the cull guard
([above](#the-terrain-fix-cull-guard)) changes the field of view the game is
told the headset shows — the game then draws the wider view itself, and EDVR
submits only the true region, copied from the game's own frame. It edits
answers, never memory: the runtime and anything else asking always receive the
truth, and it validates the runtime's projection against the shape it expects
before changing anything, standing down loudly on a mismatch. None of the three
does anything until you configure it.

**The resolution fix, off by default,** rewrites the twelve numbers that are
the width and height the game forces for the on-foot screen, in the places it
does so — nothing else: not the surrounding instructions, not the game's
decision about which screen to draw. Its safeguards, because they are the
reason to trust it:

- **No file on disk is modified.** The change exists only in memory and the
  original values are put back when the game closes.
- **It recognises the code it changes rather than trusting a version number** —
  a very specific shape: a check on which screen is being drawn, a forced 16:9
  size only in that case, the real size read from the game's data otherwise.
  That shape occurred in six places in 81 MB of code on the build this was
  developed against; EDVR accepts between three and twelve, provided they all
  agree, so an update that adds or drops one does not switch the feature off.
- **It refuses if what it finds looks wrong** — too few, too many, or places
  that disagree — and does nothing, saying so in the log.
- **It cannot corrupt the game's code.** It replaces a number with another
  number of the same size; instruction lengths and program flow are untouched.
  The worst case, if it ever matched the wrong thing, is something drawn at an
  odd size — visible, and gone on restart.
- **It says what it found before changing it**, so the log shows the resolution
  the game was really forcing.
- **All or nothing.** If any single write fails, every earlier one is undone.

For every fix here: nothing touches the network, your account, or anything the
server sees. Nothing reads or changes gameplay state — position, ship, cargo,
credits, missions. Nothing interacts with anti-cheat, and nothing attempts to
hide from anything.

If you would rather no part of this went near the game's code or memory, leave
`vscreen_res_width`/`_height` at the stock 1920x1080 (the shipped default,
which means "do not patch") and Explorer Cam unconfigured — the DLL then
behaves as earlier versions did.

## Build

Needs Visual Studio 2022 with the C++ workload and Python. Before building,
prepare the pinned official Khronos loader and notice:

```
python tools\fetch_openxr_loader.py
```

```
build.bat
```

Produces the native graphics/runtime pair, `build\openxr_loader.dll`, and
`build\smoke.exe`; the second checks the build without the game or a headset:

```
build\smoke.exe build\d3d11.dll
```

For the optional DLSS mode and an installer that carries NVIDIA's runtime, the
DLSS SDK has to be on the machine. It is not in this repository (its licence
keeps it out) and it is not in the graphics driver:

```
python tools\fetch_ngx.py
```

fetches one pinned commit of NVIDIA's public SDK repository into
`%LOCALAPPDATA%\EDVR\ngx-sdk` and checks the runtime's hash against the pin in
the script. `EDVR_NGX_SDK` points the build at a copy somewhere else.

The installer resource generation requires the native pair and bundled loader;
it does not offer a legacy OpenVR-only package. `package.bat <version>
--no-dlss` allows packaging a build made without the DLSS SDK. When the build
contains DLSS, the archive retains its matching DLL and NVIDIA license notice
alongside the installer that embeds it.

`build.bat --installer-only` rebuilds only the installer from the DLLs
already in `build\`, which the release workflow uses after the DLLs come
back from signing.

## Antivirus

A DLL that sits next to a game executable and intercepts graphics calls looks,
structurally, like something worth flagging. Some scanners will. The source is
here so you can read exactly what it does and build it yourself.

## Code signing policy

Free code signing provided by [SignPath.io](https://signpath.io/), certificate
by [SignPath Foundation](https://signpath.org/).

**What is signed.** `edvr-installer.exe`, `d3d11.dll` and `openvr_api.dll` in
every release from the first signed one on. They are built by the
[release workflow](.github/workflows/release.yml) on GitHub-hosted runners
from the tagged commit and signed by SignPath from that workflow's own
artifacts; nothing built on a developer's machine is ever signed. NVIDIA's
`nvngx_dlss.dll` and the Khronos OpenXR loader ship exactly as their
publishers sign them.

**Team.** Author, reviewer and approver:
[characterecho-sean](https://github.com/characterecho-sean). Two-factor
authentication is enabled on both the repository and SignPath.

**Privacy.** EDVR has no network code and transfers no information to
anyone. The only files it writes are in the game folder you choose and
its settings backup under `%LOCALAPPDATA%\EDVR`.

**Licence.** MIT — see [LICENSE](LICENSE).

## Licence and standing

MIT — see [LICENSE](LICENSE). One file the installer carries is not: NVIDIA's
DLSS runtime, `nvngx_dlss.dll`, is NVIDIA's software under the NVIDIA RTX SDKs
licence, distributed unmodified as part of this application as that licence
allows, and only placed on machines with an NVIDIA card. The `fsr` engine is
AMD's FidelityFX Super Resolution 3.1 upscaler through its community Direct3D
11 port (the optiscaler project's FidelityFX-SDK-DX11, MIT), compiled into
`d3d11.dll`; its notice ships as `FIDELITYFX-SDK-DX11-LICENSE.txt`.

Not affiliated with, endorsed by, or supported by Frontier Developments plc or
Valve Corporation. Elite Dangerous is a trademark of Frontier Developments plc.
Modifying the game client is not something Frontier endorses; use at your own
discretion.

**If Frontier asks for this to come down, it comes down.** No argument, no
mirrors.
