# EDVR — an unofficial patch for Elite Dangerous: Odyssey in VR

Fixes for things that make Odyssey uncomfortable in a headset.

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

*Written up in detail in [docs/eye-brightness.md](docs/eye-brightness.md) —
what causes it, how it was measured, and what a fix inside the game would be.*

**The one-frame flash when you jump or drop out of supercruise.** Once per
transition, Elite draws a single frame from the wrong viewpoint — a hard cut to
somewhere else and straight back. On a monitor it is a blink; in a headset the
visual system reads it as the world lurching. EDVR spots that frame and does not
send it to your headset, so SteamVR holds the previous frame for a moment
instead. **This one needs a second file installed — see [Install](#install).**

*Written up in detail in [docs/transition-flash.md](docs/transition-flash.md) —
three consecutive frames measured in three different coordinate spaces, and what
would fix it properly.*

**The grey haze around the on-foot screen.** On foot, Elite shows the world on a
flat screen floating in front of you, surrounded by dark grey — RGB(32,32,32).
On an OLED headset that grey is lit pixels, so the screen sits in a glowing
rectangle instead of darkness. This makes it properly black.

**The on-foot screen's distance.** Fixed by the game; adjustable here. The stock
distance is the default because it is about right — this is for anyone whose
comfort differs.

**The on-foot screen's resolution.** *Off by default — see the warning below.*
Elite renders that screen at 1920x1080 regardless of your headset, which is why
text on foot looks soft. This raises it; 4K is a clear improvement and also
applies to HMD (Cinema Mode). It costs GPU time and video memory in proportion,
and it is the one fix here that changes the game's code in memory — read
[What it does and does not do](#what-it-does-and-does-not-do) before enabling
it.

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

### The transition flash fix needs one more step

Optional, and separate because it installs differently: it **replaces** a file
the game ships rather than adding one. Everything else works without it.

The reason it cannot ride along with `d3d11.dll` is that the decision not to show
a frame has to be made where frames are handed to SteamVR, and that is
`openvr_api.dll`.

1. Close Elite Dangerous.
2. Go to
   `%LOCALAPPDATA%\Frontier_Developments\Products\elite-dangerous-odyssey-64\Openvr\win64`.
3. **Rename** the `openvr_api.dll` already there to `openvr_api_orig.dll`. Do not
   delete it — EDVR loads it and passes everything through to it.
4. Copy EDVR's `openvr_api.dll` (from the `openvr` folder of the release) into
   its place.
5. Start the game.

**Uninstall:** delete the file you copied in and rename `openvr_api_orig.dll`
back.

To check it worked, look for a second log in `edvr_logs\` with `vr` in the name.
It should say `compositor hook installed on IVRCompositor_014`. If it says the
compositor version is not one this build knows, the fix is off, the game runs
normally, and the version string it prints is worth reporting.

If you see a flash anyway, press **Pause** straight after it and send the logs —
that writes the last ten seconds of viewpoint history, which is what separates
"detected and let through" from "never detected".

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

The black-void and screen-distance fixes key off the size of the images sent to
the headset and the colour the screen's surround is cleared to, neither of which
is version specific.

**The resolution fix works the same way**, which is worth saying because it
edits code and the obvious design would not. Pinning it to one game version
would be safe and would also break it at every update, leaving it switched off
more often than on. Instead it looks for the shape of the thing it changes — a
check on which screen is being drawn, a forced 16:9 size, the real size read
from the game's data otherwise — and that shape occurs in exactly six places.
If a future version still forces the resolution this way, it will find it and
say what it found. If it does not, it does nothing and says that instead.

**The transition flash fix is the exception, and it is worth being direct about
it.** Everything above finds its target by recognising a shape. This one cannot:
the viewpoint it watches is a block of numbers in a constant buffer, and there is
no instruction pattern to recognise — only a size and an offset, both measured
from build 330683.

So it does the next best thing and checks the *data* instead of trusting the
address. A viewpoint moves, and it moves smoothly enough that a straight-line
guess from the previous two frames is usually close. EDVR watches the first 300
rendered frames and requires what it finds to behave that way. If a game update
moves that block, what is at the old offset will not move like a viewpoint, and
the fix disables itself and says so in the log rather than acting on nonsense.
It also switches itself off for the session if it ever starts withholding frames
continuously, because permanent judder would be worse than the flash.

If anything stops matching, that fix does nothing, the game renders normally,
and the log says so.

Developed against game build **330683 (4.4.0.3)**.

Frontier's launcher may remove `d3d11.dll` when it verifies the install. That is
not a fault — it has simply uninstalled EDVR. Copy the file back.

## What it does and does not do

It loads alongside the game as a `d3d11.dll` proxy, forwarding every call to
Windows' real `d3d11.dll`. If you install the transition flash fix as well, that
adds an `openvr_api.dll` proxy which forwards every call to the game's own copy.

**All but one of the fixes never touch the game.** They change how frames are
drawn from outside it: one extra copy per frame so both eyes share an exposure
value, one substituted argument to a screen-clearing call, one substituted copy
of the panel's position for two draws if you change the distance, and — for the
transition flash — reading a constant buffer the game has already filled and, on
the rare frame that was drawn from the wrong place, not forwarding one call to
SteamVR. With the resolution fix off, nothing in the game's memory is written.

**The resolution fix is different, and it is off by default.** To raise the
on-foot screen's resolution it rewrites twelve numbers in the game's code, in
memory, while it runs. Those twelve numbers are the width and height the game
forces for that screen, in the six places it does so. Nothing else is touched —
not the surrounding instructions, not the game's decision about which screen to
draw, not anything outside those twelve values.

Its safeguards, because they are the reason to trust it:

- **No file on disk is modified.** The change exists only in memory, and the
  original values are put back when the game closes.
- **It recognises the code it changes, rather than trusting a version number.**
  It looks for a very specific shape: a check on which screen is being drawn,
  a 16:9 size forced only in that case, and the real size read from the game's
  own data otherwise. That shape occurs in exactly six places in 81 MB of code.
  Identifying the thing being edited says more about whether it is the right
  thing than a version number does.
- **It refuses if what it finds looks wrong** — too few places, too many, or
  places that disagree about which resolution is being forced. Any of those and
  it does nothing and says so in the log.
- **It cannot corrupt the game's code.** It replaces a number with another
  number of the same size. Instruction lengths and program flow are untouched,
  so there is no way for it to leave the game running something that means
  anything different. The worst case, if it ever matched the wrong thing, is
  something drawn at an odd size — visible, and gone on restart.
- **It says what it found before changing it**, so the log shows the resolution
  the game was really forcing rather than one this patch assumed.
- **All or nothing.** If any single write fails, every earlier one is undone
  before it gives up. A half-applied resolution looks worse than none.

For every fix here: nothing touches the network, your account, or anything the
server sees. Nothing reads or changes gameplay state — your position, ship,
cargo, credits or missions. Nothing interacts with anti-cheat, and nothing
attempts to hide from anything.

The transition flash fix reads one thing from the game — where the renderer is
drawing from — and that is camera state, not gameplay state. It is read only, it
never writes to the buffer it reads, and the only action it can take is to not
forward a call.

If you would rather no part of this went near the game's code, leave
`vscreen_res_width` and `vscreen_res_height` at 0 and the DLL behaves exactly as
earlier versions did.

## Build

Needs Visual Studio 2022 with the C++ workload, and Python. Python is used only
to read export tables, so each proxy exports exactly what the original does.

```
build.bat
```

Produces `build\d3d11.dll` and `build\smoke.exe`. The second checks the build
without needing the game or a headset:

```
build\smoke.exe build\d3d11.dll
```

`build\openvr_api.dll` is built too, if it can find a copy of `openvr_api.dll` to
read exports from — it looks in the game's install and in `reference\`. Unlike
`d3d11.dll` that file is not a Windows component, so there is no system copy to
fall back on. Point it at one explicitly with:

```
build.bat --openvr "path\to\openvr_api.dll"
```

Without it everything except the transition flash fix still builds.

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
