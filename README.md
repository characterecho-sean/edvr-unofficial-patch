# EDVR — an unofficial patch for Elite Dangerous: Odyssey in VR

Three fixes for things that make Odyssey uncomfortable in a headset.

> **Already running EDHM or ReShade?** They install themselves as `d3d11.dll`
> too, and only one file can have that name — so don't just overwrite theirs.
> See [Running alongside other mods](#running-alongside-other-mods), which takes
> one rename and one setting.

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

## Running alongside other mods

EDHM and ReShade also install themselves as `d3d11.dll`, and only one file can
have that name. To run both:

1. **Rename** the other mod's `d3d11.dll` to something else — say
   `d3d11_edhm.dll` — leaving it in the same folder.
2. **Put EDVR's `d3d11.dll`** in its place.
3. In `edvr.ini`, under `[advanced]`, set `real_dll = d3d11_edhm.dll`.

EDVR loads that file and passes everything through it, so both mods work.
Anything the other mod doesn't handle falls through to Windows' own
`d3d11.dll`. Restart the game for it to take effect.

If the name is wrong or the file won't load, EDVR says so in the log and carries
on without it — the game still starts, you just don't get the other mod.

**One catch with EDHM.** Its uninstaller runs `del d3d11.dll`, which after this
is *EDVR's* file, not EDHM's. Running it removes EDVR and leaves EDHM's renamed
copy behind. To undo the pair cleanly: delete `d3d11.dll` and `edvr.ini`, rename
`d3d11_edhm.dll` back to `d3d11.dll`, and then use EDHM's uninstaller if you
want EDHM gone too.

<details>
<summary>Why an earlier version of this crashed, if you hit that</summary>

The first attempt loaded the other mod during `DllMain`. Windows holds the
loader lock there, and loading a DLL that isn't already in memory runs *its*
startup code under that lock — which Windows doesn't support. ReShade does real
work in its startup, so the game died instantly, before anything could be
logged.

It now loads the other mod on the first graphics call instead, once `DllMain`
has returned and the lock is released. Windows' own `d3d11.dll` is still loaded
early, so nothing is left without a destination in between.

Tested with a stand-in proxy that does work in its own `DllMain` — the exact
thing that used to crash — plus the three ways it can go wrong: a name that
doesn't exist, a file that isn't a proxy, and a setting pointed at EDVR itself.
All three fall back to the system DLL and say so.
</details>

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
