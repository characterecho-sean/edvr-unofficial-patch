# EDVR — an unofficial patch for Elite Dangerous: Odyssey in VR

Fixes for things that make Odyssey uncomfortable in a headset. Twelve fixes,
two files, about three minutes — what each fix does is under
[What it fixes](#what-it-fixes).

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

<details>
<summary><b>Or install the two files by hand</b> — the same thing, done yourself</summary>

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

The transition flash fix and Explorer Cam are applied here. It is part of the
patch rather than an extra: an install without it is one where those two fixes
are quietly absent. It installs differently from the first file, because the
game already ships a file with this name and EDVR needs that original kept:

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

</details>

### Checking it worked

Logs appear in `edvr_logs\` next to the game. There are two of them; the second,
with `vr` in the name, says
`compositor hook installed on IVRCompositor_014`. If it reports an unknown
compositor version instead, the fix is off, the game runs normally, and that
version string is worth reporting. If you see a flash anyway, press **Pause**
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
*Details: [docs/transition-flash.md](docs/transition-flash.md).*

**The missing terrain at the edges of view.** *Off by default.* Over planets,
Elite culls terrain against a narrower frustum than it renders, so squares of
ground at the edges of your view are simply not drawn — black tiles popping in
and out as you look around (Frontier issue
[72609](https://issues.frontierstore.net/issue-detail/72609)). EDVR tells the
game your headset shows a little more than it does and hands SteamVR only the
part you really see, so those tiles get drawn. Costs GPU time — about 6% at
the values tested on a Quest 3. Three settings:
[The terrain fix](#the-terrain-fix-cull-guard).
*Details: [docs/terrain-culling.md](docs/terrain-culling.md).*

**Aliasing when you supersample — the resolve at the door.** *On by
default, and idle unless the game supersamples.* When Elite renders each
eye larger than the headset asked for (its HMD Quality above 1.0), the
compositor shrinks the image on the fly as it corrects for the lenses, with
whatever sampler it happens to use. `supersample_resolve = auto` (the
default) has EDVR filter each eye down to exactly the recommended size
itself, at submit, in linear light, with a calm (Gaussian) or crisp
(Mitchell) kernel — `supersample_filter` and `supersample_width`, both live
— and hand the compositor a frame it samples one to one. The pixels are
the game's; only who filters them changes, so it cannot add detail and does
nothing unless the game is already submitting larger than asked. Costs one
small GPU pass per eye, measured by timestamp query and printed in the
graphics log: about half a millisecond per eye on a Pimax Crystal Super at
HMD Quality 1.25 (6780x6695 down to 5424x5356), a tenth of one on a Quest 3
at 1.5 (3096x3312 down to 2064x2208). `off` gives the compositor its
filtering back. For the shimmer itself:
supersample through HMD Quality rather than Elite's Supersampling slider
(the slider shrinks the image before the game's own post-processing), set
Elite's anti-aliasing to Off or SMAA and stop expecting it to touch
flicker, run anisotropic filtering at 16x, and on SteamVR turn on Advanced
Supersample Filtering.
*Details: [docs/anti-aliasing.md](docs/anti-aliasing.md).*

**The shimmer on a steady ship.** A headset's tracking never quite stops:
measured on a Pimax Crystal Super lying on a desk, the reported orientation
wanders about a tenth of an arcminute every frame, and the compositor
re-warps every frame by that motion, so any line about a pixel wide blinks
as it crosses pixel rows. A rest lock that held the render pose while the
head was still shipped on 2026-09-03 and was retired the next day: the
temporal pass below integrates that wander instead of fighting it, and the
lock could not engage on a Quest 3's tracking at all.
*Details: [docs/anti-aliasing.md](docs/anti-aliasing.md).*

**The shimmer itself — temporal anti-aliasing.** *Off by default; a first
build, flown once.* Elite has no temporal anti-aliasing, and its menu's options are
edge filters that cannot touch content flickering on and off the pixel
grid as your head moves. `temporal_aa = on` blends each frame with the
frames before it, each moved to where its content sits now, with the
projection the game renders through nudged by a sub-pixel amount every
frame so the average converges to a real supersample even with the head
held still. The first build moves the history by the headset's turn, which
is exact for the cockpit and the HUD and right for the world whenever the
ship is not turning; a fast turn leaves the world un-anti-aliased for
those frames rather than ghosted. Set Elite's own anti-aliasing to Off or
SMAA with it on. Needs a restart to turn on; the weight and the clamp are
live. Every temporal filter trades a little edge contrast for its calm, so
`render_sharpness` (0 to 1, live) runs AMD's RCAS on every outgoing frame
as the last pass at the door, for the resolve's calm kernel as much as for
this; the first flight found text a little soft without it, and 0.3 to 0.5
is where to start. The second flight found the limit of the first build:
a history moved by the head's rotation alone registers the world but not
the cockpit, whose text sits close enough that the head's translation
moves it by pixels, so it ghosts under head motion and stays soft. The
cockpit census then found the scene's own depth, and the pass now
reprojects every pixel with the head's translation through it by default,
which is what a near panel needs to hold still (three docked flights: the
history lands on the cockpit with 40% fewer clips than by rotation alone). Know the trade before
turning it on: a temporal filter converges
to a properly filtered image, which is calmer and softer than the hard,
aliased edges of text without it, and a side-by-side at HMD Quality 1.5
found the text much sharper with the resolve alone. If crisp text matters
more to you than calm edges, leave this off and keep the resolve. On an RTX
GPU, `temporal_aa = dlaa` hands the same inputs to NVIDIA's trained
history instead, and `dlss` also lets Elite's HMD Quality below 1.0 render
a fraction of the size and brings it back to full size, which buys frame
time. It does not buy text: the cockpit's panels are drawn into surfaces
that scale with the internal render size, so at HMD Quality 0.67 their
text is rasterised at two-thirds size and no upscaler recovers it. (Every
trained flight before 2026-09-04 ran with NVIDIA's history reset every
frame, a bug found by review; the verdicts on those flights are of a
spatial filter, not of DLSS.)
*Details: [docs/anti-aliasing.md](docs/anti-aliasing.md).*

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

**The planet you are scanning going black in one eye.** In the surface and
system scanners (DSS and FSS) the body renders as a featureless black disc in
the right eye and correctly in the left — silhouette intact, the blue markers
and the scanner's own UI fine in both (Frontier issue
[78021](https://issues.frontierstore.net/issue-detail/78021)). The game
issues the second eye's lighting draw with one of its inputs missing, so that
eye's lighting lands nowhere. EDVR lends the draw the input the first eye's
just used, for that one draw, put back exactly as found; `scanner_body = off`
restores the game's behaviour. Present on some machines and absent on others,
and on a machine without the bug it never engages.
*Details: [docs/scanner-body.md](docs/scanner-body.md).*

**The loading screen's shimmering ship.** The spinning ship hologram carries
a faint, low-res, head-locked pattern inside its silhouette — the hologram
is synthesized from the model's depth, and its scan pattern is sampled in
*screen* space, which a monitor can never show moving but a headset always
does. Nauseating if you focus on it. EDVR holds the pattern still for
exactly that one draw per eye; `holo_pattern = stock` restores the game's
behaviour. *Details: [docs/loading-hologram.md](docs/loading-hologram.md).*

**The sun's glare riding your head.** *On by default (`vivid`).* A star's
whole glare — corona, veiling smudge, light beams, rays, lens flare — is
drawn flat on your view like a camera overlay: it rolls when you roll your
head, the beams stay pinned horizontal to your face, and the disc tilts and
breathes as you look around. `vivid` (the default) keeps everything — beams
world-locked, the lens flare still sliding across your view — the
movie-camera look without the head coupling; `realistic` anchors only the
glow a real eye would see (corona and smudge) and removes the camera
artifacts; `sun_glare = stock` restores the game's behaviour. Works on
every star, witchspace arrivals included. Live: swap modes mid-flight
and compare. *Details: [docs/sun-glare.md](docs/sun-glare.md).*

**Smoke, steam and solar flares swimming as you look around.** *On by default
(`steady`).* Geyser plumes, the prominences that erupt off star surfaces, and
particle effects like them are drawn as flat cards all sharing
one orientation taken from the camera — so in a headset the whole plume
rolls when you tilt your head, and appears to rotate about its own axis as
you look past it. `steady` (the default) gives each particle its own
orientation, aimed at you and referenced to the world, so a plume stands
still like the volume of smoke it is meant to be. It also settles the
swimming you can see on a flat screen when you swing the mouse;
`particle_billboard = stock` restores the game's behaviour.
*Details: [docs/particle-billboards.md](docs/particle-billboards.md).*

**The on-foot screen being flat.** *Off by default.* `panel_curvature`
bends the on-foot / HMD Cinema Mode screen toward you, the way Virtual
Desktop curves its virtual display — the edges come nearer instead of
falling away. `0.3`, paired with `panel_distance = 0.7`, is a comfortable
on-foot starting point; both are live.
*Details: [docs/screen-curvature.md](docs/screen-curvature.md).*

**The grey haze around the on-foot screen.** On foot, the world is shown on a
flat screen surrounded by dark grey — lit pixels on an OLED headset, so the
screen floats in a glowing rectangle. This makes the surround properly black.

**The on-foot screen's distance.** Fixed by the game; adjustable here. The stock
distance is the default; `0.7` with the curve above is the tested pairing.

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

**The supersample resolve**, on by default and idle unless the game submits
larger than the headset asked for, is one more thing of that kind:
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
