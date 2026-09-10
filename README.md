# EDVR — an unofficial patch for Elite Dangerous: Odyssey in VR

Fixes for things that make Odyssey uncomfortable in a headset. Two dozen fixes,
two files, about three minutes — the short list is under
[What it fixes](#what-it-fixes), and each one in full is in
[docs/fixes.md](docs/fixes.md).

**It needs Elite running on its OpenVR path** — SteamVR, or an OpenXR runtime
through OpenComposite. Elite's own Oculus path is not supported; if a Meta
headset is driven by the Meta PC app over Link or Air Link, read
[Headsets and VR runtimes](#headsets-and-vr-runtimes) before you install.

**Something not working?** [Open an
issue](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/new/choose)
— that is the place a bug gets fixed, because you can attach the log, and the
log is usually the whole answer. For everything else — setup questions,
"is this normal", or just talking about it — there is a
[Discord](https://discord.gg/ynkdf6Gdua).

EDVR is free and stays free. If it improves your VR experience,
[tips are welcome](https://ko-fi.com/seancharacterecho) — please do not feel any obligation to do so.

> **Already running EDHM or ReShade?** Both can run alongside EDVR. EDHM
> installs itself as `d3d11.dll` too, and only one file can have that name —
> don't overwrite it. The installer handles this for you; by hand it is one
> rename and one setting, under
> [Running alongside other mods](#running-alongside-other-mods). ReShade needs
> nothing at all as of 0.7.2.

## Headsets and VR runtimes

**EDVR supports headsets that reach Elite through OpenVR — SteamVR, or an
OpenXR runtime by way of OpenComposite. Elite's own Oculus/Meta path is not
supported.**

Elite ships two VR back ends and picks one at launch: OpenVR, and Oculus'
native SDK. It has no OpenXR back end of its own. EDVR's second file *is*
`openvr_api.dll` — the fixes that act on a finished frame live inside the game's
own OpenVR library — so if the game does not take the OpenVR path, that half of
the patch is never loaded and everything in it is inert.

**Works:**

- **SteamVR**, and anything SteamVR drives — Valve Index, HTC Vive, Bigscreen
  Beyond, Pimax through its SteamVR driver, a Quest over **Steam Link**, and so
  on.
- **OpenXR runtimes through [OpenComposite](https://gitlab.com/znixian/OpenOVR)**
  — Virtual Desktop's VDXR, PiOpenXR, Meta's own OpenXR runtime. OpenComposite
  supplies the `openvr_api.dll` the game loads and translates to OpenXR
  underneath, so as far as Elite and EDVR are concerned it is still the OpenVR
  path. EDVR knows OpenComposite when it is there — it identifies the runtime
  beneath it by its exports, says which in the `vr` log, and has settings that
  exist only for it (`launch_centre`, `vr_handover`,
  `advanced.suppress_interfaces`).

Field-verified on a Quest 3 over Virtual Desktop and a Pimax Crystal Super over
PiOpenXR, both through OpenComposite. Real SteamVR is supported and less
measured — a log from there is a useful report either way.

**Does not work: Elite's native Oculus path.** When the Meta (Oculus) PC runtime
is what drives your headset — a Rift, a Rift S, or a Quest over Link or Air Link
with the Meta PC app — Elite prefers its own Oculus back end, loads
`LibOVRRT64_1.dll`, and never loads `openvr_api.dll` at all. Measured on a Rift
S user's machine in September 2026: a byte-perfect, correctly-placed install of
both EDVR files that the game simply never opened. **Nothing about the install
can fix this** — it is which back end the game chose, decided before EDVR has a
say.

Elite having no OpenXR back end is worth stating separately, because it is where
people look first: **Windows' OpenXR runtime selector and the Meta app's OpenXR
toggles have no bearing on any of this.** Changing them does not move Elite onto
OpenXR, because Elite never asks for OpenXR. OpenComposite is what makes an
OpenXR runtime reachable, and it does it by answering as OpenVR.

**Which one am I on?** Look in `edvr_logs\` next to the game after a session:

| What you find | What it means |
|---|---|
| Two logs, the second with `vr` in the name | The OpenVR path. Its `launch centre:` line names the runtime underneath — Valve's SteamVR or OpenComposite. The in-headset menu's **Status** page says the same. |
| One log only, and `edvr_breadcrumbs.txt` has `gfx:` lines but never a `vr:` line | The Oculus path. EDVR's `openvr_api.dll` was never loaded. |

One caution while reading that log on the Oculus path: several messages say
`openvr_api.dll is NOT installed` when the file is installed, correct, and
merely never opened. Take them as "the openvr half never ran", not as an install
fault — the breadcrumb test above is the one that decides.

**What still works there.** The fixes that live entirely in `d3d11.dll` keep
working — the exposure fix, the RemLok lines, the sun's glare, the particle
billboards, the loading hologram, the scanner body, the on-foot screen's
resolution and curvature — though a few of them lean on the other half for
per-headset tuning and fall back to their defaults without it. What goes
outright: the temporal pass and DLAA/DLSS, the supersample resolve, the
sharpening, the terrain fix, the transition flash, Explorer Cam, the in-headset
settings menu, the black void and the panel distance. That is most of the reason
to install it.

**Getting a Meta headset onto OpenVR.** Both of the usual routes bypass the Meta
PC runtime rather than arguing with it:

- **Steam Link** on Quest 2/3/Pro — streams to SteamVR directly, and the game
  finds no Oculus runtime to prefer.
- **Virtual Desktop** — either its SteamVR mode, or VDXR with OpenComposite,
  which is the configuration this project measures on a Quest 3.

With Link or Air Link and the Meta app running, Elite will keep choosing Oculus.
There is an old community workaround — running `EliteDangerous64.exe` in Windows
7 compatibility mode, which Oculus' runtime refuses, pushing the game onto
SteamVR — but it dates from the Rift CV1 era, this project has not tested it,
and it is not something to rely on. If you try it and it works, that is a useful
thing to report.

## Install

**Run `edvr-installer.exe`** from the release. One file, nothing to extract,
nothing to put in the right folder — it carries both DLLs, `edvr.ini` and
NVIDIA's DLSS runtime inside it and does the whole install:

- **Finds the game.** Frontier launcher, Steam or Epic, on whichever drive —
  it asks each launcher where it put the game rather than guessing paths, and
  confirms every answer by finding `EliteDangerous64.exe`. Or point it at a
  folder yourself.
- **Places NVIDIA's DLSS runtime** (`nvngx_dlss.dll`, 59 MB) beside the game
  when the machine has an NVIDIA card, which is what `temporal_aa = dlaa`
  needs; on any other card it skips it and says so. A copy you put there
  yourself, or one NVIDIA's own updater replaced, is left alone, and
  uninstall removes only the copy it placed.
- **Keeps your settings.** Updating never overwrites `edvr.ini`: it writes the
  new version's file with your values put back into it, so new settings and
  changed defaults arrive and nothing you tuned is lost. It says which is which
  afterwards.
- **Renames the game's `openvr_api.dll` instead of overwriting it**, which is
  the step most manual installs get wrong — and it can tell the game's own copy
  from EDVR's by reading the file, so it will not rename the wrong one.
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
only if the game is under `Program Files`, and asks at that point rather than up
front. It has no network access at all — it installs what it carries. *What it
decides and why: [docs/installer.md](docs/installer.md).*

Windows will say the program is unrecognised, because it is unsigned: **More
info → Run anyway**. Every release lists the installer's SHA-256, which is the
only provenance an unsigned binary can offer.

**Would rather place the two files yourself?**
[docs/manual-install.md](docs/manual-install.md) is the same install by hand:
where each file goes, and the `openvr_api.dll` rename that most manual installs
get wrong.

### Checking it worked

Logs appear in `edvr_logs\` next to the game. There are two of them; the second,
with `vr` in the name, says
`compositor hook installed on IVRCompositor_014`. If it reports an unknown
compositor version instead, the fix is off, the game runs normally, and that
version string is worth reporting. **If there is no second log at all**, the
game is not on its OpenVR path and half the patch never loaded — see
[Headsets and VR runtimes](#headsets-and-vr-runtimes). If you see a flash anyway, press **Pause**
straight after and send the logs — that writes the last ten seconds of viewpoint
history, which separates "detected and let through" from "never detected".

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
for "is this normal". Bugs still want an issue: chat loses attachments and
the thread, and an issue is what remembers a problem long enough to fix it.

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
  other does not (1.5 stops apart, measured; 0.4 with the fix). The planet
  that renders as a black disc in one eye in the scanners. The FSS showing
  each eye a different scan. The RemLok helmet's edge lines hanging along your
  nose instead of at your temples.
- **Things put back in the world.** A star's whole glare, which stock rolls
  and tilts with your head like a camera overlay. Geyser plumes and solar
  prominences, which swim as you look past them. The loading ship's
  head-locked scan pattern. The launch movie, off its 27-degree rectangle and
  onto the splash screen's own surface.
- **The one-frame flash** each time you jump or drop out of supercruise —
  detected and not sent, so the runtime holds the previous frame instead.
- **Shimmer and sharpness.** Temporal anti-aliasing, with DLAA and DLSS on RTX
  cards, includes UI and smoke depth and rotating-station motion automatically.
  Choose it and the **DLSS preset** (default K) on Performance; AA remains off
  by default. RCAS sharpening is available separately. Supersample filtering
  is experimental and off by default.
- **The terrain missing at the edges of view** over planets — Elite culls
  against a narrower frustum than it renders, so squares of ground go
  undrawn. Off by default; it costs about 6% GPU at the tested values.
- **On foot:** the grey surround made properly black, the screen moved, bent
  and raised above its forced 1920x1080, and Explorer Cam, which gives you a
  real stereo view of your commander in the external camera.

**Each fix in full — what it costs, what it is measured at, and which setting
turns it off — is in [docs/fixes.md](docs/fixes.md).** The defaults are what
most people want; the exceptions are called out there and in the in-headset
menu.

## Explorer Cam

On foot, Elite renders the world once, flat, and shows that image to both eyes —
there is no depth because none is being drawn. The external camera renders in
proper stereo, so that is where Explorer Cam works: it moves your viewpoint to
your commander's head while you are in that camera. **It cannot make first
person 3D** — the flat screen stays flat — and it does not try.

**It replaces one camera preset: Commander Right Shoulder.** On that preset the
camera sits at your commander's head instead of the preset's usual framing;
cycle to it for the 3D view, off it for normal framing. Every other preset is
untouched. `advanced.head_offset_view` selects a different preset to give up instead.

### It gives you no capability you do not already have

This matters more than the effect does. The external camera is Elite's own
feature, opened with your own binding, and inside it you cannot act: no
shooting, scanning, opening a panel, using a terminal, or picking anything up.
To act you switch back to first person, exactly as you do today. Explorer Cam
changes **where the camera is while you are already in that mode** and nothing
else — no extra reach, nothing revealed the camera was not already showing, no
step removed that anyone else has to take. A player with it and a player without
it can do the same things, in the same order, with the same clicks. One of them
is looking at it in 3D.

It touches nothing shared: no network path, no server state, nothing another
player observes, and no gameplay data read or written.

### Setting it up

1. **Hotkeys: nothing to do.** EDVR reads your external-camera and
   next-camera-view keys straight from your Elite key configuration — the
   *on-foot* camera binding, which Elite keeps separate from the ship's.
   If they are on keyboard keys, you are done: the log's first lines name
   the keys it adopted and the file they came from. Rebind them in Elite,
   even mid-session, and EDVR follows within a few seconds. EDVR only
   *watches* these keys; it never presses them or interferes with the game
   receiving them.

   They matter because on screen, entering the camera looks identical to
   boarding your ship — the camera key is how EDVR knows which it was. And
   near a planet the game rebuilds its camera data every few seconds, so the
   next-view key's presses are what carry "which preset am I on" through the
   gaps.

   If your camera is bound **only to a controller**, bind a keyboard key for
   it in Elite (Options → Controls) for now — EDVR watches the keyboard, and
   controller support is planned.

2. Get on foot, open the camera, and cycle to **Commander Right Shoulder** —
   two presses from the view the camera opens on. That is the preset the
   offset replaces; every other preset keeps its normal framing.

3. Tune the offsets with the headset on; they reload about once a second:

   ```
   head_offset_right   = -0.25   + is to your commander's right
   head_offset_up      = 0.25    + is up
   head_offset_forward = 1.25    + is the way your commander faces
   ```

   These are tuned for Commander Right Shoulder, which already sits close to
   your commander and faces the way they face — so the numbers are small, and
   the negative `right` brings you off the shoulder onto the centre line. Pick a
   preset several metres further back and `forward` becomes the large one, two
   to three metres instead of one. Starting points, not universal answers.

**Comfort.** These move the viewpoint of a headset you are wearing; change them
a little at a time. Entering and leaving is a cut rather than a glide, because
the game's own camera change is already a cut.

### What it does under the hood

Two things the other fixes do not:

- **It changes the headset position the game is told about.** Each frame the
  game asks SteamVR where your head is; EDVR adds your offset to the answer. The
  game then moves its *own* camera — as far as Elite knows, you leaned. That is
  what makes it work: culling and object placement follow.
- **It reads one number from the game's memory:** which external-camera view is
  showing, so the offset applies to the right preset. To find where that number
  lives it searches once, on the first frame you are on foot, for a marker
  identifying the camera settings — it keeps nothing but the small view index,
  skips the game's code, and never writes.
- **It reads event names from the game's journal** — the documented file Elite
  writes for third-party tools in Saved Games — to know when gameplay has
  started, when you step onto your feet (where the game resets its camera
  view), and when a jump begins and resolves. Names only (`LoadGame`,
  `Disembark`, `StartJump`, `FSDJump`, `SupercruiseEntry`); no other content
  is read or kept, and `d3d11.journal_watch = 0` turns it off entirely.

Its safeguards, because they are the reason to trust it:

- **Nothing happens without your camera key**, as above — and EDVR only
  *watches* that key; it never presses or sends it.
- **Your viewpoint moves at most 10 m per axis.** Beyond that it clamps and says
  so — refusing outright would snap the view, which is worse when worn.
- **It counts your camera-key presses**, and since build 332753 that is all it
  does. Reading the preset out of the game is off by default — see below.
- **It expires.** The two halves of EDVR agree once a frame about which mode you
  are in; if the deciding half stops running, the half that moves your view
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

For Frontier issue
[72609](https://issues.frontierstore.net/issue-detail/72609) — "Culling of
planet surface in VR too aggressive", the black squares at the edges of view
over planets. What was measured, why the fix works from outside the game, and
what a fix inside it would look like:
[docs/terrain-culling.md](docs/terrain-culling.md). It is off by default
because it costs GPU time; enabling it is three settings in `edvr.ini`.

1. **Turn it on** — live, like every other cull-guard setting:

   ```
   [fix]
   cull_guard = symmetric
   ```

2. **Gate it to your headset** (recommended). The `vr` log prints your
   headset's signature — `cull guard: this headset's signature is 94x99` —
   copy that value in:

   ```
   cull_guard_headsets = 94x99
   ```

   The guard then runs only on that headset. On a rig that swaps headsets,
   the other one pays nothing, with no ini edits at swap time.

3. **Pick the margin.** Left alone the guard covers the full shortfall —
   guaranteed wherever the fix works at all, and the most expensive (~48%
   more rendered pixels on a Quest 3). The values tested on a Quest 3 keep
   the edges clean at about **6%**:

   ```
   cull_guard_fraction_h = 0.25
   cull_guard_fraction_v = 0
   ```

   Both are live — save the file mid-flight and the guard picks them up. If
   black squares persist on your headset, raise `_h` in steps; the log's
   `cull guard margins` line names what each step leaves uncovered.

Working, the `vr` log says `cull guard stage 1`, then two `cull guard LIVE`
lines. `cull guard INERT` means this runtime shapes its projections in a way
the guard refuses to edit — the game runs normally, and that log is worth
attaching to an issue. Field-verified on Quest 3 via Virtual Desktop (where
the missing tiles reproduced, and are gone) and Pimax via PiOpenXR; real
SteamVR is unmeasured so far, so a log from there is a useful report either
way.

## Settings

Everything is in `edvr.ini`, next to the game; with the file missing you get the
defaults. `black_void`, `panel_distance` and the Explorer Cam offsets reload
while the game runs; the rest need a restart.

**The in-headset menu.** Press **F8** in the game (the key is `hotkey.menu`)
and a settings panel appears where you are looking, anchored in the world so it
stays put while you read it. Up and Down pick a row; Left and Right change it;
Enter toggles; Tab changes page; Escape closes. While it is open the game sees
no keyboard at all, so none of those keys reach the ship -- your HOTAS and
mouse still do. Every change is written to `edvr.ini` and applies the way a
hand edit would, and a row that only takes effect at the next launch says so.
The **Monitor** page is fpsVR's readout -- frame rate and 1% low, the app's
and the compositor's GPU time, dropped and reprojected frames, CPU, GPU, VRAM
and RAM -- with a frame-time strip; `menu.fps_overlay = on` pins a one-line
version of it to your view while the menu is closed. `menu.developer = on`
adds the advanced and experimental sections. The whole design is in
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

**If something has gone wrong** and the log has not answered it — a launch crash on
0.7.1 or earlier, every fix except the brightness one going quiet at once, or
an old EDHM pairing that crashed —
[docs/troubleshooting.md](docs/troubleshooting.md) has the three faults with a
known cause and what each one needs.

## Game updates

**The brightness fix finds its target by what it does**, not by which version of
Elite compiled it: the calculation that writes the exposure result exactly twice
per frame, stable for five frames running, before it acts. The black-void and
screen-distance fixes key off image sizes and a clear colour, neither version
specific. **The resolution fix** looks for the shape of the code it changes
rather than trusting a version number — its safeguards are described
[below](#what-it-does-and-does-not-do).

**Two things are measured from a specific build** (330683 / 4.4.0.3, the one
this was developed against), and both degrade rather than guess. The 4.4.1.0
update (build 332753) moved the second of them and left the first alone —
[docs/build-332753.md](docs/build-332753.md) has the full re-check:

- **The transition flash fix** watches a viewpoint in a constant buffer — no
  instruction pattern to recognise, only a size and an offset. So it checks the
  *data*: a viewpoint moves smoothly, and EDVR requires the first 300 rendered
  frames to behave that way before acting. If an update moves the block, what is
  at the old offset will not move like a viewpoint, and the fix disables itself
  and says so. It also switches off for the session if it ever withholds
  continuously — permanent judder would be worse than the flash.
- **Explorer Cam's camera marker** will also move on update — and did, in
  332753. Reading the preset is now off by default and Explorer Cam counts key
  presses instead; see its section above for what that costs.

**Recurring false jumps are recognised and left alone.** Flying low over
terrain, the game alternates between shadow cameras whose fixed separation looks
like an enormous jump — measured at ~568,000 units, recurring for eight minutes,
each withhold felt as judder. A jump that keeps recurring at the same size is a
distance between render passes, not a transition — real transitions vary,
because real motion does — so the first of a size is withheld and matching ones
are left alone. `transition_flash_repeat_percent` controls it; the ini notes why
raising `transition_flash_units` cannot help (the false jumps are *larger* than
real ones, not smaller). Both eyes of a frame also follow one verdict, decided
at whichever eye submits first. A `transition flash so far:` line counts
withheld and recognised separately when something changes; no such line means it
never fired.

Frontier's launcher may remove `d3d11.dll` when it verifies the install — that
is not a fault, it has simply uninstalled EDVR. Copy the file back.

## What it does and does not do

It loads alongside the game as a `d3d11.dll` proxy, forwarding every call to
Windows' real `d3d11.dll`; the `openvr_api.dll` proxy forwards every call to the
game's own copy.

**Most of the fixes never touch the game.** They change how frames are drawn
from outside it: four small copies per frame so both eyes share an exposure
value, one substituted argument to a screen-clearing call, one substituted copy
of the panel's position if you change the distance, and — for the transition
flash — reading a constant buffer the game has already filled and, on the rare
frame drawn from the wrong place, not forwarding one call to SteamVR. That read
is camera state, not gameplay state; it never writes to the buffer it reads, and
the only action it can take is to not forward a call — or to hand SteamVR the
game's own previous frame in its place: a copy EDVR keeps of the last frame it
forwarded, always the game's content, never EDVR's.

**The supersample resolve** is experimental and off by default:
when the game submits a larger frame than the headset asked for, one GPU
filter pass shrinks the game's frame into a texture EDVR owns, and that copy
is what SteamVR receives — the game's texture is read, never written, no
answer the game asks for changes, and nothing is read from memory. The
temporal pass and the sharpening (`temporal_aa`, `render_sharpness`, both
off by default) are two more passes of exactly that kind, except that the
temporal pass also shifts the projection the game is told by a fraction of
a pixel each frame, the way the terrain fix shifts it by a margin.

**Three fixes do more, and each is described in full:** the resolution fix
(below) rewrites twelve numbers in the game's code; Explorer Cam
([above](#explorer-cam)) reads one number from the game's memory and changes
the headset position the game is told about; the cull guard
([above](#the-terrain-fix-cull-guard)) changes the field of view the game is
told the headset shows — the game then draws the wider view itself, and EDVR
submits only the true region, copied from the game's own frame. It edits
answers, never memory: the runtime and anything else asking always receive
the truth, and it validates the runtime's projection against the shape it
expects before changing anything, standing down loudly on a mismatch. None
of the three does anything until you configure it.

**The resolution fix, off by default,** rewrites the twelve numbers that are the
width and height the game forces for the on-foot screen, in the places it does
so — nothing else: not the surrounding instructions, not the game's decision
about which screen to draw. Its safeguards, because they are the reason to
trust it:

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
`vscreen_res_width`/`_height` at the stock 1920x1080 (the shipped default, which
means "do not patch") and Explorer Cam unconfigured — the DLL then behaves as
earlier versions did.

## Build

Needs Visual Studio 2022 with the C++ workload, and Python (used only to read
export tables, so each proxy exports exactly what the original does).

```
build.bat
```

Produces `build\d3d11.dll` and `build\smoke.exe`; the second checks the build
without the game or a headset:

```
build\smoke.exe build\d3d11.dll
```

For the DLAA and DLSS modes, and for an installer that carries NVIDIA's
runtime, the DLSS SDK has to be on the machine. It is not in this repository
(its licence keeps it out) and it is not in the graphics driver:

```
python tools\fetch_ngx.py
```

fetches one pinned commit of NVIDIA's public SDK repository into
`%LOCALAPPDATA%\EDVR\ngx-sdk`, a single copy every checkout and worktree
finds, and checks the runtime's hash against the pin in the script. A build
without it still succeeds and says so loudly: it has no DLAA and its
installer carries no runtime. `package.bat` refuses to package such a build
unless told `--no-dlss`. `EDVR_NGX_SDK` points the build at a copy somewhere
else.

`build\openvr_api.dll` is built too if a copy of the real file can be found to
read exports from — it looks in the game's install and `reference\`, or point at
one explicitly:

```
build.bat --openvr "path\to\openvr_api.dll"
```

Without it, everything except the flash fix and Explorer Cam still builds.

## Antivirus

A DLL that sits next to a game executable and intercepts graphics calls looks,
structurally, like something worth flagging. Some scanners will. The source is
here so you can read exactly what it does and build it yourself.

## Licence and standing

MIT — see [LICENSE](LICENSE). One file the installer carries is not: NVIDIA's
DLSS runtime, `nvngx_dlss.dll`, is NVIDIA's software under the NVIDIA RTX SDKs
licence, distributed unmodified as part of this application as that licence
allows, and only placed on machines with an NVIDIA card.

Not affiliated with, endorsed by, or supported by Frontier Developments plc or
Valve Corporation. Elite Dangerous is a trademark of Frontier Developments plc.
Modifying the game client is not something Frontier endorses; use at your own
discretion.

**If Frontier asks for this to come down, it comes down.** No argument, no
mirrors.
