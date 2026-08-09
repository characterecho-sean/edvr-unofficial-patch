# EDVR — an unofficial patch for Elite Dangerous: Odyssey in VR

Three fixes for things that make Odyssey uncomfortable in a headset.

> **⚠️ Do not install this if you already use EDHM or ReShade.** They also
> install themselves as `d3d11.dll`, only one file can have that name, and
> overwriting theirs will break them. An attempt at running both crashed the
> game on startup and has been removed. See [Compatibility](#compatibility).

## What it fixes

**One eye going darker than the other near bright lights.** Elite works out how
bright the scene is separately for each eye. Near a star or a station floodlight
one eye can see the light and the other cannot, so one eye stops down and the
other does not. The result is two eyes disagreeing about how bright the world
is — uncomfortable, and hard to unsee once noticed.

Measured on a held view at a star: the eyes differed by about **1.5 stops**
without the fix and about **0.4** with it. What remains is the glow around the
star in the eye that can actually see it, which is correct.

**The grey haze around the on-foot screen.** On foot, Elite shows the world on a
flat screen floating in front of you, surrounded by dark grey — RGB(32,32,32).
On an OLED headset that grey is lit pixels, so the screen sits in a glowing
rectangle instead of darkness. This makes it properly black.

**The on-foot screen's distance.** Fixed by the game; adjustable here. The stock
distance is the default because it is about right — this is for anyone whose
comfort differs.

## Install

1. Close Elite Dangerous.
2. Check there is no `d3d11.dll` already next to `EliteDangerous64.exe`. If
   there is, stop — see [Compatibility](#compatibility).
3. Copy `d3d11.dll` and `edvr.ini` into the folder containing
   `EliteDangerous64.exe`, normally
   `%LOCALAPPDATA%\Frontier_Developments\Products\elite-dangerous-odyssey-64`.
4. Start the game.

Press **Scroll Lock** in game to toggle the brightness fix on and off. Look at a
star with one eye dimmed and press it — the difference is immediate.

**Uninstall:** delete those two files.

## Settings

Everything is in `edvr.ini`, next to the game. With the file missing you get the
defaults, which are what most people want. `black_void` and `panel_distance` can
be changed while the game runs; the rest need a restart.

## Compatibility

**EDVR cannot currently run alongside EDHM or ReShade.** All three work by being
the `d3d11.dll` next to the game, and only one file can have that name.

A setting to chain through another one existed briefly and **crashed the game on
startup**, before it could write anything to its log. The cause is structural
rather than a small bug: loading another mod's DLL has to happen while Windows
holds the loader lock, and that runs the other mod's startup code at a point
where Windows does not support it. Making it work means loading the other mod
later and re-pointing everything afterwards, which is a real design and needs
testing against an actual EDHM install before it goes near anyone's game.

Until then, one or the other. Sorry.

## Game updates

**The brightness fix finds its target by what it does, not by which version of
Elite compiled it.** It looks for the calculation that writes the exposure
result and runs exactly twice per frame, once per eye, and requires that to hold
for five frames running before it acts. Identifying it by a fixed fingerprint
instead would break on every game update.

The other two fixes key off the size of the images sent to the headset and the
colour the screen's surround is cleared to, neither of which is version
specific.

If anything stops matching, that fix does nothing, the game renders normally,
and the log says so.

Developed against game build **330683 (4.4.0.3)**.

Frontier's launcher may remove `d3d11.dll` when it verifies the install. That is
not a fault — it has simply uninstalled EDVR. Copy the file back.

## What it does and does not do

It loads alongside the game as a `d3d11.dll` proxy, forwarding every call to
Windows' real `d3d11.dll`. It changes three things about how frames are drawn:
one extra copy per frame so both eyes share an exposure value, one substituted
argument to a screen-clearing call, and — only if you change the distance — one
substituted copy of the panel's position for two draws.

It does not modify, patch or write to the game's memory or any game file. It
does not touch the network, your account, or anything the server sees. It does
not read or change gameplay state, and it does not interact with anti-cheat.

## Build

Needs Visual Studio 2022 with the C++ workload, and Python. Python is used only
to read the export table of your system `d3d11.dll`, so the proxy exports
exactly what the original does.

```
build.bat
```

Produces `build\d3d11.dll` and `build\smoke.exe`. The second checks the build
without needing the game or a headset:

```
build\smoke.exe build\d3d11.dll
```

## Antivirus

A DLL that sits next to a game executable and intercepts graphics calls looks,
structurally, like something worth flagging. Some scanners will. The source is
here so you can read exactly what it does and build it yourself.

## Licence and standing

MIT — see [LICENSE](LICENSE).

Not affiliated with, endorsed by, or supported by Frontier Developments plc or
Valve Corporation. Elite Dangerous is a trademark of Frontier Developments plc.
Modifying the game client is not something Frontier endorses; use at your own
discretion.

**If Frontier asks for this to come down, it comes down.** No argument, no
mirrors.
