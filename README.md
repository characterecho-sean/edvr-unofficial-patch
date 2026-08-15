# EDVR — an unofficial patch for Elite Dangerous: Odyssey in VR

Fixes for things that make Odyssey uncomfortable in a headset. Six fixes, two
files, about three minutes — what each fix does is under
[What it fixes](#what-it-fixes).

> **Already running EDHM?** It installs itself as `d3d11.dll` too, and only one
> file can have that name — don't overwrite it. See
> [Running alongside other mods](#running-alongside-other-mods): one rename and
> one setting. **ReShade currently cannot run alongside EDVR** — same section,
> short explanation.

## Install

Two files. The first enables most of the fixes; the second is needed by the
transition flash fix and Explorer Cam.

### The first file — `d3d11.dll`

1. Close Elite Dangerous.
2. Check there is no `d3d11.dll` already next to `EliteDangerous64.exe`. If
   there is, stop — see [Running alongside other mods](#running-alongside-other-mods).
3. Copy `d3d11.dll` and `edvr.ini` into the folder containing
   `EliteDangerous64.exe`, normally
   `%LOCALAPPDATA%\Frontier_Developments\Products\elite-dangerous-odyssey-64`.
4. Start the game.

Press **Scroll Lock** in game to toggle the brightness fix; at a star the
difference is immediate.

### The second file — `openvr_api.dll`

Needed by the transition flash fix and Explorer Cam; the other four fixes work
without it. It installs differently, because the game already ships a file with
this name and EDVR needs that original kept:

> **Do not overwrite or delete the game's `openvr_api.dll`. Rename it.** EDVR
> loads the renamed original and passes every call through to it. If the
> original is overwritten instead, EDVR has nothing to forward to — VR will not
> start, and the log will say exactly this. (If that happens: verify the install
> in Frontier's launcher to restore the file, and redo the steps below.)

1. Close Elite Dangerous.
2. Go to
   `%LOCALAPPDATA%\Frontier_Developments\Products\elite-dangerous-odyssey-64\Openvr\win64`.
3. **Rename** the `openvr_api.dll` already there to `openvr_api_orig.dll`.
4. Copy EDVR's `openvr_api.dll` (from the release's `openvr` folder) into its
   place.
5. Start the game.

The reason this fix cannot ride along in `d3d11.dll`: the decision not to show a
frame has to be made where frames are handed to SteamVR, and that is this file.
Skipping it leaves the flash fix able to detect and log only, and **Explorer Cam
doing nothing at all** — EDVR says so in the log the first time it would have
engaged.

### Checking it worked

Logs appear in `edvr_logs\` next to the game. With the second file installed, a
second log with `vr` in the name says
`compositor hook installed on IVRCompositor_014`. If it reports an unknown
compositor version instead, the fix is off, the game runs normally, and that
version string is worth reporting. If you see a flash anyway, press **Pause**
straight after and send the logs — that writes the last ten seconds of viewpoint
history, which separates "detected and let through" from "never detected".

### Uninstall

Delete `d3d11.dll` and `edvr.ini` from the game folder; in `Openvr\win64`,
delete EDVR's `openvr_api.dll` and rename `openvr_api_orig.dll` back.

## What it fixes

**One eye going darker than the other near bright lights.** Elite meters scene
brightness separately per eye, so near a star or a floodlight one eye stops down
and the other does not — two eyes disagreeing about how bright the world is.
Measured at a held view of a star: about **1.5 stops** apart without the fix,
**0.4** with it; what remains is the glow in the eye that can actually see the
star, which is correct. *Details: [docs/eye-brightness.md](docs/eye-brightness.md).*

**The one-frame flash when you jump or drop out of supercruise.** Once per
transition, Elite draws a single frame from the wrong viewpoint. On a monitor it
is a blink; in a headset it reads as the world lurching. EDVR spots that frame
and does not send it, so SteamVR holds the previous frame for a moment instead.
**Needs the second file.**
*Details: [docs/transition-flash.md](docs/transition-flash.md).*

**The grey haze around the on-foot screen.** On foot, the world is shown on a
flat screen surrounded by dark grey — lit pixels on an OLED headset, so the
screen floats in a glowing rectangle. This makes the surround properly black.

**The on-foot screen's distance.** Fixed by the game; adjustable here. The stock
distance is the default because it is about right.

**The on-foot screen's resolution.** *Off by default.* Elite renders that screen
at 1920x1080 regardless of headset, which is why on-foot text looks soft. This
raises it — any 16:9 size from 640 to 8192 wide; **2880x1620**, **3200x1800**,
**3840x2160** and **5120x2880** are the useful ones (the last is ~1.8x the
memory of 4K). It costs GPU time and video memory in proportion, and it is the
one fix that changes the game's code in memory — read
[What it does and does not do](#what-it-does-and-does-not-do) first.

**On foot not being in 3D — Explorer Cam.** First person on foot is a flat image
shown to both eyes; the external camera renders real stereo. Explorer Cam puts
your viewpoint at your commander's head while you are in that camera, so the
surface, your ship, and the room you are standing in have depth. Off until you
configure it, and worth the few minutes — see [Explorer Cam](#explorer-cam).

## Explorer Cam

On foot, Elite renders the world once, flat, and shows that image to both eyes —
there is no depth because none is being drawn. The external camera renders in
proper stereo, so that is where Explorer Cam works: it moves your viewpoint to
your commander's head while you are in that camera. **It cannot make first
person 3D** — the flat screen stays flat — and it does not try.

**It replaces one camera preset: Commander Rear Profile.** On that preset the
camera sits at your commander's head instead of the preset's usual framing;
cycle to it for the 3D view, off it for normal framing. Every other preset is
untouched. `fix.head_offset_view` selects a different preset to give up instead.

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

1. **Install the second file** if you have not — [Install](#install) above.
   Explorer Cam is applied in `openvr_api.dll`; without it nothing below has any
   effect.

2. Set `hotkey.external_camera` in `edvr.ini` to **your** Elite external-camera
   binding. Combinations work — Elite's default is `CTRL+ALT+SPACE`:

   ```
   external_camera = CTRL+ALT+SPACE
   ```

   **Nothing happens until you do this.** Deliberate: on screen, entering the
   camera looks identical to boarding your ship, and guessing wrong would move
   your viewpoint inside your own cockpit.

3. Get on foot, open the camera, and cycle to **Commander Rear Profile**. It is
   not the preset the camera opens on. EDVR reads which preset you are on from
   the game — nothing else to bind.

4. Tune the offsets with the headset on; they reload about once a second:

   ```
   head_offset_right   = 0.0     + is to your commander's right
   head_offset_up      = 0.8     + is up
   head_offset_forward = 2.75    + is the way your commander faces
   ```

   `forward` is the large one — the camera starts several metres behind your
   commander. These are starting points, not universal answers.

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

Its safeguards, because they are the reason to trust it:

- **Nothing happens without your camera key**, as above — and EDVR only
  *watches* that key; it never presses or sends it.
- **Your viewpoint moves at most 10 m per axis.** Beyond that it clamps and says
  so — refusing outright would snap the view, which is worse when worn.
- **It gives up rather than guessing.** If the search finds nothing, or the
  value stops looking like a camera view, it reports "do not know" and falls
  back to counting your camera-key presses. It never substitutes a number that
  might be wrong.
- **It expires.** The two halves of EDVR agree once a frame about which mode you
  are in; if the deciding half stops running, the half that moves your view
  stops trusting it within about a second and puts your viewpoint back.
- **It degrades on game updates.** The marker will move when Elite updates; EDVR
  then finds nothing, says so in the log, and falls back to key counting.
  `camera_index_type_offset` can be corrected by hand without waiting for a
  build.

## Settings

Everything is in `edvr.ini`, next to the game; with the file missing you get the
defaults. `black_void`, `panel_distance` and the Explorer Cam offsets reload
while the game runs; the rest need a restart.

## Running alongside other mods

**EDHM** also installs as `d3d11.dll`. To run both:

1. **Rename** EDHM's `d3d11.dll` — say `d3d11_edhm.dll` — leaving it in place.
2. **Put EDVR's `d3d11.dll`** in its place.
3. In `edvr.ini`, under `[advanced]`, set `real_dll = d3d11_edhm.dll`.

EDVR passes everything through it; anything it doesn't handle falls through to
Windows' own `d3d11.dll`. Restart to take effect. If the name is wrong or the
file won't load, EDVR says so in the log and carries on without it.

**One catch with EDHM:** its uninstaller runs `del d3d11.dll`, which after this
is *EDVR's* file. To undo the pair cleanly: delete `d3d11.dll` and `edvr.ini`,
rename `d3d11_edhm.dll` back, then run EDHM's uninstaller if wanted.

**ReShade is different, and currently cannot run alongside EDVR.** EDHM is a
proxy that chains; ReShade wraps the graphics objects themselves, and the two
approaches collide — either load order fails. This is a known limitation with a
planned fix, not a configuration problem: no rename or setting makes it work
today, and the attempt costs nothing worse than ReShade's effects not loading.

<details>
<summary>Why an earlier version of this crashed, if you hit that</summary>

The first attempt loaded the other mod during `DllMain`, where Windows holds the
loader lock; loading a DLL that isn't already in memory runs *its* startup code
under that lock, which Windows doesn't support. It now loads the other mod on
the first graphics call instead. Tested with a stand-in proxy that does work in
its own `DllMain` — the exact thing that used to crash — plus the three ways it
can go wrong: a missing name, a non-proxy file, and a setting pointed at EDVR
itself. All three fall back to the system DLL and say so.
</details>

## Game updates

**The brightness fix finds its target by what it does**, not by which version of
Elite compiled it: the calculation that writes the exposure result exactly twice
per frame, stable for five frames running, before it acts. The black-void and
screen-distance fixes key off image sizes and a clear colour, neither version
specific. **The resolution fix** looks for the shape of the code it changes
rather than trusting a version number — its safeguards are described
[below](#what-it-does-and-does-not-do).

**Two things are measured from a specific build** (330683 / 4.4.0.3, the one
this was developed against), and both degrade rather than guess:

- **The transition flash fix** watches a viewpoint in a constant buffer — no
  instruction pattern to recognise, only a size and an offset. So it checks the
  *data*: a viewpoint moves smoothly, and EDVR requires the first 300 rendered
  frames to behave that way before acting. If an update moves the block, what is
  at the old offset will not move like a viewpoint, and the fix disables itself
  and says so. It also switches off for the session if it ever withholds
  continuously — permanent judder would be worse than the flash.
- **Explorer Cam's camera marker** will also move on update; the fix then
  reports "do not know", falls back to key counting, and
  `camera_index_type_offset` can be corrected by hand — see its section above.

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
Windows' real `d3d11.dll`; the optional `openvr_api.dll` proxy forwards every
call to the game's own copy.

**Most of the fixes never touch the game.** They change how frames are drawn
from outside it: four small copies per frame so both eyes share an exposure
value, one substituted argument to a screen-clearing call, one substituted copy
of the panel's position if you change the distance, and — for the transition
flash — reading a constant buffer the game has already filled and, on the rare
frame drawn from the wrong place, not forwarding one call to SteamVR. That read
is camera state, not gameplay state; it never writes to the buffer it reads, and
the only action it can take is to not forward a call.

**Two fixes do more, and each is described in full:** the resolution fix
(below) rewrites twelve numbers in the game's code; Explorer Cam
([above](#explorer-cam)) reads one number from the game's memory and changes the
headset position the game is told about. Neither does anything until you
configure it.

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

MIT — see [LICENSE](LICENSE).

Not affiliated with, endorsed by, or supported by Frontier Developments plc or
Valve Corporation. Elite Dangerous is a trademark of Frontier Developments plc.
Modifying the game client is not something Frontier endorses; use at your own
discretion.

**If Frontier asks for this to come down, it comes down.** No argument, no
mirrors.
