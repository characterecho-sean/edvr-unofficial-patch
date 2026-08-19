# EDVR — an unofficial patch for Elite Dangerous: Odyssey in VR

Fixes for things that make Odyssey uncomfortable in a headset. Nine fixes,
two files, about three minutes — what each fix does is under
[What it fixes](#what-it-fixes).

**Something not working?** [Open an
issue](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/new/choose)
— that is the place a bug gets fixed, because you can attach the log, and the
log is usually the whole answer. For everything else — setup questions,
"is this normal", or just talking about it — there is a
[Discord](https://discord.gg/hhDSxU4nX).

> **Already running EDHM or ReShade?** Both can run alongside EDVR. EDHM
> installs itself as `d3d11.dll` too, and only one file can have that name —
> don't overwrite it. See
> [Running alongside other mods](#running-alongside-other-mods): one rename and
> one setting. ReShade needs nothing at all as of 0.7.2.

## Install

Two files. The first enables most of the fixes; the second is needed by the
transition flash fix and Explorer Cam.

### The first file — `d3d11.dll`

1. Close Elite Dangerous.
2. Check there is no `d3d11.dll` already next to `EliteDangerous64.exe`. If
   there is, stop — see [Running alongside other mods](#running-alongside-other-mods).
3. Copy `d3d11.dll` and `edvr.ini` into **the folder containing
   `EliteDangerous64.exe`**. Where that is depends on how you installed the
   game — these are the ones people have reported:

   | Install | Folder |
   |---|---|
   | Frontier launcher | `…\Frontier\EDLaunch\Products\elite-dangerous-odyssey-64` (often under `Program Files (x86)`, and not always on C:) |
   | Steam | `…\steamapps\common\Elite Dangerous\Products\elite-dangerous-odyssey-64` |
   | Epic | `…\Epic Games\EliteDangerous\Products\elite-dangerous-odyssey-64` |

   If none of those match, find `EliteDangerous64.exe` yourself — that folder
   is the answer, whatever its path. EDVR writes the folder it loaded from
   into the first lines of its log, so you can always check afterwards.
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
2. Find **the folder that already contains the game's own `openvr_api.dll`**.
   Start from the `Openvr` folder next to `EliteDangerous64.exe`: on some
   installs the file sits directly in `Openvr\`, on others in `Openvr\win64\`.
   Whichever one holds it is the right folder — there is no single correct
   path, so go by the file rather than the name.
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

### Reporting a problem

**Open an issue**, and attach `edvr_logs\` — both files if there are two. The
log carries the build stamp, which fixes it were able to install, and what
each one decided, so a report with it attached is usually diagnosable in one
pass. Without it there is very little to go on. If the game will not start at
all, `edvr_breadcrumbs.txt` next to `EliteDangerous64.exe` is written
unbuffered and survives a crash that eats the log — send that.

The [Discord](https://discord.gg/hhDSxU4nX) is good for setup questions and
for "is this normal". Bugs still want an issue: chat loses attachments and
the thread, and an issue is what remembers a problem long enough to fix it.

### Uninstall

Delete `d3d11.dll` and `edvr.ini` from the game folder; in whichever `Openvr`
folder you used, delete EDVR's `openvr_api.dll` and rename
`openvr_api_orig.dll` back.

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

**The missing terrain at the edges of view.** *Off by default.* Over planets,
Elite culls terrain against a narrower frustum than it renders, so squares of
ground at the edges of your view are simply not drawn — black tiles popping in
and out as you look around (Frontier issue
[72609](https://issues.frontierstore.net/issue-detail/72609)). EDVR tells the
game your headset shows a little more than it does and hands SteamVR only the
part you really see, so those tiles get drawn. Costs GPU time — about 6% at
the values tested on a Quest 3. **Needs the second file.** Three settings:
[The terrain fix](#the-terrain-fix-cull-guard).
*Details: [docs/terrain-culling.md](docs/terrain-culling.md).*

**The RemLok helmet's edge lines hanging along your nose.** When the
emergency helmet deploys, its faint edge lines end up in the middle of your
view instead of at your temples — the game stamps the same both-edges
overlay into each eye with no per-eye placement (Frontier issue
[69074](https://issues.frontierstore.net/issue-detail/69074)). EDVR clips
each eye to the line on its own outward side, which is what a real helmet
looks like; `remlok_lines = hide` removes the lines entirely, `stock`
restores the game's behaviour, and because the game parks the lines at the
lens rim, `remlok_line_angle` (default 46°) places them at a visible angle
derived per headset from its real projection. Field-verified on a Pimax via
OpenComposite; if any rig shows the lines back on the nose, `remlok_swap_eyes`
in the ini is the one-line correction, and that log is worth an issue.
*Details: [docs/remlok-lines.md](docs/remlok-lines.md).*

**The loading screen's shimmering ship.** The spinning ship hologram carries
a faint, low-res, head-locked pattern inside its silhouette — the hologram
is synthesized from the model's depth, and its scan pattern is sampled in
*screen* space, which a monitor can never show moving but a headset always
does. Nauseating if you focus on it. EDVR holds the pattern still for
exactly that one draw per eye; `holo_pattern = stock` restores the game's
behaviour. *Details: [docs/loading-hologram.md](docs/loading-hologram.md).*

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

**It replaces one camera preset: Commander Right Shoulder.** On that preset the
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

2. **Hotkeys: nothing to do.** EDVR reads your external-camera and
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

3. Get on foot, open the camera, and cycle to **Commander Right Shoulder** —
   two presses from the view the camera opens on. That is the preset the
   offset replaces; every other preset keeps its normal framing.

4. Tune the offsets with the headset on; they reload about once a second:

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
  started and when you step onto your feet, which is where the game resets its
  camera view. Names only (`LoadGame`, `Disembark`); no other content is read
  or kept, and `d3d11.journal_watch = 0` turns it off entirely.

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

## The terrain fix (cull guard)

For Frontier issue
[72609](https://issues.frontierstore.net/issue-detail/72609) — "Culling of
planet surface in VR too aggressive", the black squares at the edges of view
over planets. What was measured, why the fix works from outside the game, and
what a fix inside it would look like:
[docs/terrain-culling.md](docs/terrain-culling.md). It is off by default
because it costs GPU time; enabling it is three settings in `edvr.ini`, and
it needs [the second file](#the-second-file--openvr_apidll).

1. **Turn it on, then restart the game** — this is the one cull-guard
   setting that is not live:

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

**ReShade** needs no configuration — install it the way ReShade tells you to
(normally as `dxgi.dll`) and EDVR composes with it. Both mods' effects apply.

<details>
<summary>If you are on 0.7.1 or earlier and the game crashes on launch</summary>

Update to 0.7.2. Before it, EDVR intercepted Direct3D calls by copying an
object's method table and pointing *the object itself* at the copy — which
quietly re-pointed objects ReShade owns and dispatches through, and the game
crashed while EDVR was installing
([#6](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/6)).
It presented as intermittent because EDVR's crash sentinel disables the
Direct3D fixes on the launch after a crash, so it alternated. EDVR now swaps
the individual method pointers where they already live and never touches the
object.
</details>

<details>
<summary>If everything except the exposure fix stopped working</summary>

Look in `edvr_gfx_*.log` for the periodic `vScreen totals:` line. If
**`largest eye-draw count`** is `0` and stays `0` while you are actually
flying or on foot, that one number is the whole fault: the black void, the
panel distance, the transition flash fix and Explorer Cam all read it, and a
zero switches all four off at once. The exposure fix does not read it, which
is why it keeps working and makes the rest look individually broken.

EDVR decides which render targets are your eye textures. Before 0.7.3 it
guessed by size — 2048×2048 or larger, minus anything exactly the size of the
on-foot panel. Two things defeated that guess, both silently:

- **A panel raised to exactly your eye-texture size.** The panel exclusion
  then removed the eyes along with the panel. This is the one to suspect if
  you set `vscreen_res_width`/`_height` to `3840`/`2160`.
- **Eye textures under 2048 on an axis**, which never qualified at all. This
  is most headsets: a Quest 3 through SteamVR renders about 1832×1920 or
  1728×1824 per eye at ordinary settings, and only clears 2048 on both axes
  near or above its native panel resolution.

From 0.7.3, `openvr_api.dll` reads the size of the texture the game actually
submits and tells the graphics side, so it matches your real eye textures
instead of guessing, and says so in both logs. If the count is still stuck at
0, EDVR now prints a line naming every target size it did see — please attach
it to an issue. As an immediate workaround on any version, set
`vscreen_res_width`/`_height` to `2880`/`1620` (short enough that nothing
collides) or back to `1920`/`1080`.
</details>

<details>
<summary>Why an earlier version of this crashed with EDHM, if you hit that</summary>

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
the only action it can take is to not forward a call — or to hand SteamVR the
game's own previous frame in its place: a copy EDVR keeps of the last frame it
forwarded, always the game's content, never EDVR's.

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
