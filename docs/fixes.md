# What EDVR fixes

Every fix in one place, in one or two sentences each. The pattern behind almost
all of them is the same: something that is correct on a monitor is wrong in a
headset — drawn once for two eyes, pinned to your face instead of standing in
the world, or sized for a screen you are not looking at.

Each entry names its setting in `edvr.ini` and the value it ships with. Nearly
all of them are also a row in the in-headset menu (**F8**), and most are live —
change them and the running game picks them up within about a second. Where a
fix has a full write-up — what was measured, why the game does it that way, and
what a fix inside the engine would look like — it is linked at the end of the
entry.

Two files carry these. `d3d11.dll` holds the ones that change how a frame is
drawn; `openvr_api.dll` holds the ones that act on the finished frame on its way
to the headset. An install missing the second file is missing those, and both
files need Elite running on its OpenVR path — see
[Headsets and VR runtimes](../README.md#headsets-and-vr-runtimes).

The last two sections cover how the fixes survive a game update and what each
one touches in the game, with the safeguards on those that change its code or
memory.

---

## The two eyes disagreeing

**One eye going darker than the other near bright lights.** Elite meters scene
brightness separately per eye, so near a star or a floodlight one eye stops down
and the other does not. EDVR copies one eye's exposure into both. Measured at a
held view of a star: about **1.5 stops** apart without it, **0.4** with — and
what remains is the glow in the eye that can actually see the star, which is
correct. `fix.share_exposure = 1` (on). *[eye-brightness.md](eye-brightness.md).*

**The planet you are scanning going black in one eye.** In the surface and
system scanners the body renders as a featureless black disc in the right eye
and correctly in the left — silhouette intact, markers and scanner UI fine in
both (Frontier issue
[78021](https://issues.frontierstore.net/issue-detail/78021)). The game issues
the second eye's lighting draw with one of its inputs missing. EDVR lends that
draw the input the first eye just used, for that one draw, put back exactly as
found. Present on some machines and absent on others; on a machine without the
bug it never engages. `fix.scanner_body = on`.
*[scanner-body.md](scanner-body.md).*

**The FSS showing each eye a different scan.** In the Full System Scanner the
zoomed body's not-yet-resolved tiles can be hard black in one eye and already
filled in the other, so the resolve animation splits (a stock bug, reproduced
clean). EDVR fills the left eye's black pixels from the right at your headset's
own optical-infinity disparity, inside the scanner screen's measured rectangle
only, and freezes the composite's inputs so the second eye cannot diverge by
construction. Needs `openvr_api.dll`. `fix.fss_eye_sync = on`.
*[fss-scanner.md](fss-scanner.md), and the vendor report in
[frontier-fss-bug-report.md](frontier-fss-bug-report.md).*

**The RemLok helmet's edge lines hanging along your nose.** When the emergency
helmet deploys, the game stamps the same both-edges overlay into each eye with
no per-eye placement, so its faint lines end up in the middle of your view
instead of at your temples (Frontier issue
[69074](https://issues.frontierstore.net/issue-detail/69074)). EDVR clips each
eye to the line on its own outward side. Because the game parks the lines at the
lens rim, `fix.remlok_line_angle` (default 46°) places them at a visible angle
derived per headset from its real projection; `hide` removes them entirely and
`stock` restores the game's behaviour. `fix.remlok_lines = outer`.
*[remlok-lines.md](remlok-lines.md).*

---

## Drawn on your face instead of in the world

**The one-frame flash when you jump or drop out of supercruise.** Once per
transition Elite draws a single frame from the wrong viewpoint. On a monitor it
is a blink; in a headset it reads as the world lurching. EDVR spots that frame
and does not send it, so the runtime holds the previous frame for a moment
instead. Needs `openvr_api.dll` — without it the flash is detected and logged
but not withheld. `fix.transition_flash = 1` (on).
*[transition-flash.md](transition-flash.md).*

**The sun's glare riding your head.** A star's whole glare — corona, veiling
smudge, light beams, rays, lens flare — is drawn flat on your view like a camera
overlay: it rolls when you roll your head, the beams stay pinned horizontal to
your face, and the disc tilts as you look around. `vivid` (the default) keeps
every element but world-locks it, which is the movie-camera look without the
head coupling; `realistic` keeps only the glow a real eye would see and drops
the camera artifacts; `stock` restores the game's behaviour. Works on every
star, witchspace arrivals included, and is live, so you can swap mid-flight and
compare. `fix.sun_glare = vivid`. *[sun-glare.md](sun-glare.md).*

**Smoke, steam and solar flares swimming as you look around.** Geyser plumes,
the prominences that erupt off star surfaces and particle effects like them are
flat cards that all share one orientation taken from the camera, so the whole
plume rolls when you tilt your head and appears to spin about its own axis as
you look past it. `steady` gives each particle its own orientation, aimed at you
and referenced to the world. It also settles the swimming you can see on a flat
screen when you swing the mouse. `fix.particle_billboard = steady`.
*[particle-billboards.md](particle-billboards.md).*

**The loading screen's shimmering ship.** The spinning ship hologram carries a
faint, low-res, head-locked pattern inside its silhouette: the hologram is
synthesized from the model's depth and its scan pattern is sampled in *screen*
space, which a monitor can never show moving and a headset always does.
Nauseating if you focus on it. EDVR holds the pattern still for exactly that one
draw per eye. `fix.holo_pattern = steady`.
*[loading-hologram.md](loading-hologram.md).*

---

## Shimmer and sharpness

**The night-vision pulse line moving with your head.** Keep the pulse at
a stable distance while retaining the game's terrain shading and
brightness. `fix.night_vision_stability = 1` stays on by default and
works with AA Off, TAA or DLSS. **Realistic nightvision** is a separate
experimental appearance, off by default
(`experimental.night_vision_realistic = 0`), with its brightness control
under Experimental too. Disabling that appearance keeps pulse stability.

**Temporal anti-aliasing and upscaling.** Select TAA, DLSS or FSR on
Performance (`fix.temporal_aa = off` by default); every mode includes UI and
smoke depth, adaptive UI history and moving ships', vehicles' and settlement
parts' exact motion from Elite's own records automatically (stations and ships
in space are not yet verified). On an RTX card, the
automatic NVIDIA choice displays **DLAA** at HMD Quality 1.0 or higher and
**DLSS** below it, with **DLSS preset** (or **DLAA preset**) on the same page,
default **K** (`fix.temporal_aa_model = k`), and J, L, M or Automatic choices.
**FSR** is AMD's FidelityFX Super Resolution 3.1 upscaler at the same sizes
DLSS would use; it runs on any GPU, NVIDIA's included, and needs no runtime
file beside the game (`fix.temporal_aa = fsr`).
*[anti-aliasing.md](anti-aliasing.md).*

**HUD and panel text swimming under the trained modes.** Elite draws its
interface depth-tested but never writes its depth, so a temporal pass holds a
panel a metre from your face at infinity and the history rejects it under every
head movement. EDVR captures the depth of panel strokes, dim comms icons and
targeting markers into private buffers for temporal AA, preserving the game's
scene depth and smoke; this follows the AA mode automatically, with no separate
UI-depth switch.

**Softness after any of the above.** Every temporal filter and every calm
resolve kernel trades a little edge contrast for its calm. `fix.render_sharpness`
(0 to 1, live) runs AMD's RCAS on every outgoing frame as the last pass at the
door; 0.3 to 0.5 is where to start. `fix.render_sharpness = 0.0` (off).
*[anti-aliasing.md](anti-aliasing.md).*

---

## Over a planet

**Terrain missing at the edges of view.** *Off by default.* Over planets Elite
culls terrain against a narrower frustum than it renders, so squares of ground
at the edges of your view are simply not drawn — black tiles popping in and out
as you look around (Frontier issue
[72609](https://issues.frontierstore.net/issue-detail/72609)). EDVR tells the
game your headset shows a little more than it does and hands the runtime only
the part you really see, so those tiles get drawn. It costs GPU time — about 6%
at the values tested on a Quest 3, more if you leave the margin at full — which
is why it is off. `fix.cull_guard = off`.
*[terrain-culling.md](terrain-culling.md).*

Turning it on takes three settings in `edvr.ini`, and it wants to be gated to
your headset:

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

**Frame rate dropping at busy settlements.** *Off by default.* At a crowded
settlement Elite draws tens of thousands of small parts a frame, enough on its
own to hold the frame over the headset's refresh rate — the cheapest thing to
give back is distant detail. `auto` lowers settlement detail only while the
frame runs long, a step at a time, and gives it back once there is headroom;
back to the game's own detail the moment you leave the settlement. `reduced`
keeps detail down the whole time you are at a settlement, whether or not the
frame is running long. Parts beyond about 100 m thin out and can pop as it
steps; cockpit only for now. Measured at one settlement: 45-50 fps to 70-80,
with no visible change from the cockpit. `fix.settlement_detail = game`
(default, the game's own detail), `auto` or `reduced`.

---

## On foot

**The grey haze around the on-foot screen.** On foot the world is shown on a
flat screen surrounded by dark grey — lit pixels on an OLED headset, so the
screen floats in a glowing rectangle. This makes the surround properly black.
`fix.black_void = 1` (on).

**The screen's distance.** Fixed by the game, adjustable here. `0.7` is the
tested pairing with the curve below. `fix.panel_distance = 1.0` (stock).
*[screen-curvature.md](screen-curvature.md).*

**The screen being flat.** *Off by default.* Bends the on-foot / HMD Cinema Mode
screen toward you, the way Virtual Desktop curves its virtual display — the
edges come nearer instead of falling away. `0.3` with `panel_distance = 0.7` is
a comfortable starting point. `fix.panel_curvature = 0.0`.
*[screen-curvature.md](screen-curvature.md).*

**The screen's resolution.** *Auto by default.* Elite renders that screen at
1920x1080 regardless of headset, which is why on-foot text looks soft. EDVR
now sizes it itself — 125% of the width your headset actually rendered per
eye last session, rounded to a clean 16:9 pair — so it tracks a sharper VR
render resolution one restart later, with nothing to set. Type a width in
pixels instead to pick your own; the height always follows it at 16:9 and is
not a separate setting any more. Any 16:9 width from 640 to 8192 works;
**2880**, **3200**, **3840** and **5120** are the useful ones. It costs GPU
time and video memory in proportion, and it changes the game's code in
memory: read [What the fixes touch](#what-the-fixes-touch) first.
`fix.vscreen_res_width = auto` (default); `1920` turns it off, same as stock.

**On foot not being in 3D — Explorer Cam.** First person on foot is a flat image
shown to both eyes; the external camera renders real stereo. Explorer Cam puts
your viewpoint at your commander's head while you are in that camera, so the
surface, your ship and the room have depth. It cannot make first person 3D and
does not try, and it gives you no capability you do not already have. Off until
you configure it, and worth the few minutes:
[Explorer Cam](explorer-cam.md). `fix.head_offset_*`, unset.

---

## Launch and loading

**The launch movie on a small rectangle pinned to your face.** Elite plays it at
1024x576, about 27 degrees wide, head-locked. `screen` plays it on the splash's
own virtual screen instead — world-anchored on the game's forward, sized to what
the splash after it covers, so the cut between them just works — resampled first
with AMD's FSR so the magnification does not read as pixelation. How sharp it
can get is your on-foot screen resolution above. The world anchor needs
`openvr_api.dll`; without it the movie stays head-locked and the log says so.
`skip` does not play it at all: the game's open of the ident file is answered
"not found", the answer renaming the file gives, so it goes straight to the
splash and nothing on disk is touched. `fix.intro_video = screen`.
*[intro-video.md](intro-video.md).*

**The banding behind the intro and the menu.** That backdrop is stored in an old
compressed format and is very dark, so its gradients come out stepped —
magnified across a headset's field of view the steps read as blocks and patches.
`splash` keeps the nicer of the two pictures the game already loaded (the big
splash it shows first and then throws away) and smooths the banding out of it
once, when it first draws. It is the game's own art either way; nothing is added
to your install. `fix.intro_backdrop = splash`.
*[menu-backdrop.md](menu-backdrop.md).*

**Loading dialogs dimming your entire view.** While a loading dialog is up the
game lays a frosted wash and a full-view tint over everything, which is ordinary
on a monitor and oppressive in a headset, covering artwork far beyond the
dialog's edges. `screen` moves the dimming where it belongs: the wash and the
full-view tint go, and the splash screen steps back by the same tint instead,
exactly while a dialog is up. The dialog's own box, border and text are
untouched. Intro only — it retires for the session the moment a rendered scene
arrives. `fix.loading_dim = screen`.
*[loading-panel-handoff.md](loading-panel-handoff.md).*

---

## Two you can turn off because you want to

Neither is a bug. Both ship as the game's own behaviour and exist because
someone asked.

**The hyperspace tunnel's starfield.** `off` leaves the tunnel empty, so a jump
is the swirling walls and nothing streaking past inside them. The draw is
withheld, not corrupted, so nothing else about the jump changes.
`fix.witchspace_stars = on` (stock).

**The high-wake marker flashing under the speed readout.** It pulses in time
with the target indicator's blue flash because both are drawn into the same
cockpit holo panel. `off` drops that one draw and leaves the rest of the panel
alone. `fix.wake_pulse = off`.

---

## Surviving a game update

The brightness fix finds its target by what it does, not by which version of
Elite compiled it: it looks for the calculation that writes the exposure result
exactly twice per frame, and waits for that to hold for five frames running
before it acts. The black-void and screen-distance fixes key off image sizes
and a clear colour, and neither is version specific. The resolution fix looks
for the shape of the code it changes; its safeguards are described
[below](#what-the-fixes-touch).

Two things are measured from a specific build (330683 / 4.4.0.3, the one this
was developed against), and both degrade instead of guessing. The 4.4.1.0
update (build 332753) moved the second of them and left the first alone;
[build-332753.md](build-332753.md) has the full re-check:

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
  instead; [explorer-cam.md](explorer-cam.md) says what that costs.

The transition flash fix also recognises recurring false jumps and leaves them
alone. Flying low over terrain, the game alternates between shadow cameras
whose fixed separation looks like an enormous jump (measured at ~568,000 units,
recurring for eight minutes, each withhold felt as judder). A jump that keeps
recurring at the same size is a distance between render passes, not a
transition, because real transitions vary as real motion does. So the first
jump of a size is withheld and matching ones are left alone.
`transition_flash_repeat_percent` controls this. Raising
`transition_flash_units` cannot help, because the false jumps are *larger* than
real ones. Both eyes of a frame follow one verdict, decided at whichever eye
submits first. When something changes, a `transition flash so far:` line counts
withheld and recognised jumps separately; if there is no such line, the fix
never fired.

---

## What the fixes touch

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
(below) rewrites twelve numbers in the game's code. The settlement detail fix,
set to `auto` or `reduced`, hooks the game's own detail setter and changes one
number, the game's level-of-detail distance. `ui_quality` (off by default)
sizes panels inside the game's own panel code, and `static_prop_updates` (off
by default) hooks the game's update of settlement structures and props and
skips it for those that have not changed. `intro_video = skip` answers the
game's open of the launch movie with "not found" through its import table; the
default, `screen`, does not. Some advanced settings, all off by default, hook
the game for diagnosis or experiments, and `edvr.ini` describes each. Explorer
Cam ([explorer-cam.md](explorer-cam.md)) changes the headset position the game
is told about; its read of one number from the game's memory is off by default
(`camera_index_track = 0`). The cull guard
([above](#over-a-planet)) changes the field of view the game is told the
headset shows; the game then draws the wider view itself, and EDVR submits only
the true region, copied from the game's own frame. The cull guard edits
answers, never memory, so the runtime and anything else that asks always
receive the truth, and before changing anything it validates the runtime's
projection against the shape it expects, standing down loudly on a mismatch.
Explorer Cam and the cull guard do nothing until you configure them.

Two changes are always made, and no setting turns them off. At load EDVR
redirects two of the game's imports in memory: `LoadLibraryW`, so that Elite's
attempt to load the Oculus runtime (LibOVR) fails and the game takes the OpenVR
path EDVR serves, and `DirectInput8Create`, so that the game sees no keyboard
while the in-headset menu is open. Every other library load, and all input
while the menu is closed, pass through unchanged.

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

If you would rather EDVR changed as little of the game as possible, set
`vscreen_res_width` to `1920` (the stock size, meaning "do not patch"), keep
`settlement_detail` at `game`, `intro_video` at `screen` and
`camera_index_track` at `0`, and leave `temporal_aa`, `ui_quality`,
`static_prop_updates` and the advanced settings off. The two import redirects
above still apply, because they are how EDVR takes over VR startup and the
menu's keyboard.
