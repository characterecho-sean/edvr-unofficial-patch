# EDVR: an unofficial patch for Elite Dangerous in VR

DLSS and FSR upscaling for Elite in VR, and two dozen fixes for things that
make Odyssey uncomfortable in a headset, in a native OpenXR package that takes
about three minutes to install. [Upscaling: DLSS and
FSR](#upscaling-dlss-and-fsr) covers the upscalers. The short list of fixes is
under [What it fixes](#what-it-fixes), and [docs/fixes.md](docs/fixes.md)
describes each one in full.

If something is not working, [open an
issue](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/new/choose).
Bugs get fixed there because you can attach the log, and the log usually shows
the cause. The [Discord](https://discord.gg/ynkdf6Gdua) is for setup questions,
"is this normal", or just talking about it; chat loses attachments and threads,
so bug reports belong in an issue.

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

Elite still starts VR through its OpenVR-facing path, and EDVR keeps that entry
point for compatibility; behind it, EDVR's native runtime owns the OpenXR
session. The bundled Khronos loader reaches whichever runtime Windows has set
as its active OpenXR runtime, whether SteamVR, PiOpenXR, VDXR or Meta's, and
all of them run on the same native EDVR backend. The package has no backend to
choose and never falls back to the legacy EDVR pair; Elite's legacy LibOVR path
is not supported, even as a fallback. The release installer checks the Elite
executable's profile and refuses an unsupported game revision before it writes
anything.

Before launching Elite, set the Windows active OpenXR runtime to the one you
intend to use, since that is the one the bundled loader picks, and make sure
that runtime is available and your headset connected. EDVR needs no SteamVR
loader; do not install OpenComposite or a vendor OpenXR loader to make it work.
Then launch Elite as you normally would.

## Install

**Download `edvr-installer-<version>.zip` from the release, unzip it, and run
`edvr-installer.exe`.** It is a single file once unzipped, with nothing else
to extract or put in the right folder, and it carries the native
graphics/runtime pair, the bundled OpenXR loader and its notice, `edvr.ini`,
and the optional DLSS runtime. It finds the game and keeps your settings and
other mods working. Before it changes anything, it shows you exactly what it is
about to do and waits for a yes, and it copies every file it replaces into
`edvr_backup\` first. [docs/installer.md](docs/installer.md) covers its buttons
and explains what it decides and why.

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

If something has gone wrong and the log has not answered it,
[docs/troubleshooting.md](docs/troubleshooting.md) covers the faults with a
known cause and what each one needs, among them a game that dies a second or
two after launch and VR that fails to start after an EDVR update.

### Uninstall

Run `edvr-installer.exe` and press **Uninstall**. It removes EDVR's files and
renames the game's `openvr_api.dll` back. If another mod was chained behind
EDVR, it also puts that mod back under its own name, the step a manual
uninstall usually forgets. It leaves your `edvr.ini` alone unless you ask for
it to go, so a reinstall finds your settings again.

To uninstall by hand, delete `d3d11.dll` and `edvr.ini` from the game folder,
then, in whichever `Openvr` folder you used, delete EDVR's `openvr_api.dll` and
rename `openvr_api_orig.dll` back.

## Upscaling: DLSS and FSR

EDVR can hand each frame to an upscaler that calms the shimmer: NVIDIA's
trained DLSS on an RTX card, or AMD's FSR 3.1, which runs on any GPU and needs
no extra file. With Elite's HMD Quality below 1.0 the game renders smaller and
DLSS brings it back to size, which is what saves frame time; FSR works at the
same sizes. At 1.0 or above DLSS runs at your full render size, which is DLAA.

**Important** Do *not* change the Supersample Quality (leave at 1.0).

Pick DLSS or FSR in the Anti-aliasing row on the in-headset menu's Performance
page, or set `fix.temporal_aa = dlss` or `fsr`. It is off by default and takes
effect at the next frame; set Elite's own anti-aliasing to Off or SMAA with it
on. DLSS needs NVIDIA's runtime, which the installer places on a machine with
an NVIDIA card; without an RTX card or the runtime it runs EDVR's own TAA
instead and says so in the log. The DLSS preset, on the same page, is K by
default (`fix.temporal_aa_model = k`). Sharpening, also on that page
(`fix.render_sharpness`, 0 to 1, off by default), runs AMD's RCAS to hand back
some of the edge contrast the temporal pass trades for calm; 0.3 is a fair
start.

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
- For shimmer and frame time there is DLSS or FSR, under [Upscaling: DLSS and
  FSR](#upscaling-dlss-and-fsr).
- Over planets, Elite culls against a narrower frustum than it renders, so
  squares of ground at the edges of view go undrawn. The fix is off by default
  and costs about 6% GPU at the tested values.
- At busy settlements, `fix.settlement_detail = auto` thins distant detail only
  while the frame runs long, in the cockpit for now. It is off by default; at
  one settlement it took the frame rate from 45-50 fps to 70-80 with no visible
  change.
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

It takes over one camera preset, Commander Right Shoulder, and gives you no
capability you do not already have: inside the external camera you cannot act,
and Explorer Cam changes only where the camera is.
[docs/explorer-cam.md](docs/explorer-cam.md) covers setting it up and what it
does under the hood, safeguards included.

## The terrain fix (cull guard)

This is for Frontier issue
[72609](https://issues.frontierstore.net/issue-detail/72609), "Culling of
planet surface in VR too aggressive": the black squares at the edges of view
over planets. It is off by default because it costs GPU time, and turning it on
takes three settings in `edvr.ini`, which
[docs/fixes.md](docs/fixes.md#over-a-planet) walks through.

## Settings

Everything is in `edvr.ini` next to the game, and if the file is missing you
get the defaults. Most settings take effect while the game runs, within about a
second; `edvr.ini` marks the ones that need a game restart.

Press **F8** in the game (the key is `hotkey.menu`) to open the in-headset
menu: a settings panel appears where you are looking, anchored in the world so
it stays put while you read it. Up and Down pick a row, Left and Right change
it, Enter toggles, Tab changes page and Escape closes. The keys you already use
to walk Elite's own cockpit panels work in the menu too, read from your Elite
bindings (`hotkey.read_game_bindings`), and the panel's bottom line names them.
While the menu is open the game sees no keyboard at all, though your HOTAS and
mouse still do. Every change is written to `edvr.ini` and applies the way a
hand edit would, and a row that only takes effect at the next launch says so.

The Monitor page is fpsVR's readout with a frame-time strip, and
`menu.fps_overlay = on` pins a one-line version of it to your view while the
menu is closed. `menu.developer = on` adds the advanced and experimental
sections. The whole design is in
[docs/settings-menu.md](docs/settings-menu.md).

## Running alongside other mods

**The installer does this for you**: it recognises what is in the `d3d11.dll`
slot, renames it, writes the setting, and tells you what it did. EDVR then
passes everything through EDHM.
[docs/manual-install.md](docs/manual-install.md#running-alongside-edhm-by-hand)
has the same procedure by hand.

**EDHM's uninstaller runs `del d3d11.dll`**, which after this is *EDVR's* file.
If that has happened, leaving EDVR gone and EDHM still parked under the renamed
file, the installer's Repair recognises it and puts both back.

ReShade needs no configuration. Install it the way ReShade tells you to
(normally as `dxgi.dll`) and EDVR composes with it, so both mods' effects
apply.

## Game updates

The brightness, black-void, screen-distance and resolution fixes find their
targets by what the game's code does or what its frames look like, whichever
version of Elite compiled it. The two things measured from one build (330683 /
4.4.0.3) degrade instead of guessing, as
[docs/fixes.md](docs/fixes.md#surviving-a-game-update) describes.

Frontier's launcher may remove `d3d11.dll` when it verifies the install.
Nothing is broken when it does: it has simply uninstalled EDVR, so copy the
file back.

## What it does and does not do

EDVR loads alongside the game as a `d3d11.dll` proxy that forwards every call
to Windows' real `d3d11.dll`, and its `openvr_api.dll` is EDVR's own OpenXR
runtime. Most of the fixes work on the frames and never touch the game. A few
change the running game in memory: by default the resolution fix rewrites
twelve numbers in the game's code, and two of the game's imports are always
redirected so that EDVR handles VR startup and the menu's keyboard. DLSS, FSR
and TAA hook the game's object updates while they are on.
[docs/fixes.md](docs/fixes.md#what-the-fixes-touch) lists each change, its
safeguards, and which settings turn it off.

No fix here touches the network, your account, or anything the server sees, and
none reads or changes gameplay state (position, ship, cargo, credits,
missions). None interacts with anti-cheat, and none attempts to hide from
anything.

## Build

Building needs Visual Studio 2022 with the C++ workload, and Python.
[docs/building.md](docs/building.md) has the steps.

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
