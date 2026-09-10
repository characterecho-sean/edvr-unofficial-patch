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

**Aliasing when you supersample — the resolve at the door.** When Elite renders
each eye larger than the headset asked for (HMD Quality above 1.0), the
compositor shrinks the image on the fly while it corrects for the lenses, with
whatever sampler it happens to use. EDVR filters each eye down to exactly the
recommended size itself, at submit, in linear light, and hands the compositor a
frame it samples one to one. The pixels are the game's — it cannot add detail,
and it does nothing at HMD Quality 1.0 or below. Costs one small GPU pass per
eye: about half a millisecond per eye on a Pimax Crystal Super at 1.25, a tenth
of one on a Quest 3 at 1.5. `experimental.supersample_resolve = auto` (on),
kernel `fix.supersample_filter = calm` (Gaussian) or `crisp`
(Mitchell — sharper, a touch of ringing). *[anti-aliasing.md](anti-aliasing.md).*

**The shimmer itself — temporal anti-aliasing.** *Off by default.* Elite has no
temporal anti-aliasing, and its menu's options are edge filters that cannot
touch content flickering on and off the pixel grid as your head moves. `on`
blends each frame with the frames before it, each reprojected to where its
content sits now through the scene's own depth, with the projection nudged a
sub-pixel amount every frame so the average converges to a real supersample even
with your head still. On an RTX card, `dlaa` hands the same inputs to NVIDIA's
trained history instead, and `dlss` also lets HMD Quality below 1.0 render
smaller and brings it back to full size, which buys frame time. Set Elite's own
anti-aliasing to Off or SMAA with any of them. **Know the trade:** a temporal
filter converges to a properly filtered image, which is calmer *and softer* than
hard aliased text — a side-by-side at HMD Quality 1.5 found text sharper with
the resolve alone. If crisp text matters more to you than calm edges, leave this
off and keep the resolve. `fix.temporal_aa = off`.
*[anti-aliasing.md](anti-aliasing.md).*

**HUD and panel text swimming under the trained modes.** Elite draws its
interface depth-tested but never writes its depth, so a temporal pass holds a
panel a metre from your face at infinity and the history rejects it under every
head movement. EDVR has the holo panels and the flight HUD write their depth
into the buffer the pass reads, and the text holds still. Nothing happens while
`temporal_aa` is off. `fix.ui_depth = on`.
*[crisp-ui-handoff.md](crisp-ui-handoff.md).*

**Softness after any of the above.** Every temporal filter and every calm
resolve kernel trades a little edge contrast for its calm. `fix.render_sharpness`
(0 to 1, live) runs AMD's RCAS on every outgoing frame as the last pass at the
door; 0.3 to 0.5 is where to start. `fix.render_sharpness = 0.0` (off).
*[anti-aliasing.md](anti-aliasing.md).*

**The shimmer on a steady ship — retired.** A headset's tracking never quite
stops: on a Pimax Crystal Super lying on a desk the reported orientation wanders
about a tenth of an arcminute a frame, and the compositor re-warps by that
motion, so any line about a pixel wide blinks as it crosses pixel rows. A rest
lock that held the render pose while the head was still shipped 2026-09-03 and
was retired the next day — the temporal pass above integrates that wander
instead of fighting it, and the lock could not engage on a Quest 3's tracking at
all. *[anti-aliasing.md](anti-aliasing.md).*

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
is why it is off. Three settings, and it wants to be gated to your headset:
[The terrain fix](../README.md#the-terrain-fix-cull-guard).
`fix.cull_guard = off`. *[terrain-culling.md](terrain-culling.md).*

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

**The screen's resolution.** *Off by default.* Elite renders that screen at
1920x1080 regardless of headset, which is why on-foot text looks soft. This
raises it — any 16:9 size from 640 to 8192 wide; **2880x1620**, **3200x1800**,
**3840x2160** and **5120x2880** are the useful ones. It costs GPU time and video
memory in proportion, and it is the one fix that changes the game's code in
memory: read [What it does and does not
do](../README.md#what-it-does-and-does-not-do) first.
`fix.vscreen_res_width = 1920`, `fix.vscreen_res_height = 1080` (both stock,
meaning "do not patch").

**On foot not being in 3D — Explorer Cam.** First person on foot is a flat image
shown to both eyes; the external camera renders real stereo. Explorer Cam puts
your viewpoint at your commander's head while you are in that camera, so the
surface, your ship and the room have depth. It cannot make first person 3D and
does not try, and it gives you no capability you do not already have. Off until
you configure it, and worth the few minutes:
[Explorer Cam](../README.md#explorer-cam). `fix.head_offset_*`, unset.

---

## Launch and loading

**The launch movie on a small rectangle pinned to your face.** Elite plays it at
1024x576, about 27 degrees wide, head-locked. `screen` plays it on the splash's
own virtual screen instead — world-anchored on the game's forward, sized to what
the splash after it covers, so the cut between them just works — resampled first
with AMD's FSR so the magnification does not read as pixelation. How sharp it
can get is your on-foot screen resolution above. The world anchor needs
`openvr_api.dll`; without it the movie stays head-locked and the log says so.
`fix.intro_video = screen`. *[intro-video.md](intro-video.md).*

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

**The launch movie freezing part-way through, on OpenComposite.** OpenComposite
throws away the session it started with and builds a new one for the game's
graphics card, about two and a half seconds, and stock that lands inside the
game's first compositor call — part-way through the movie. `early` pays it
before the game asks for the compositor at all, so the movie plays from its
start; the pause is the same length but nothing is drawing yet. Free on SteamVR,
which has no session to rebuild. Off by default because a crash was reported on
a third-party rig this project cannot test — if the game crashes during startup,
put it back to `stock` and please report it. `fix.vr_handover = stock`.

**Your play space in the wrong place, on OpenComposite.** OpenComposite puts the
seated origin somewhere different each launch — sometimes in front of you,
sometimes beside you, sometimes behind. `auto` recentres on OpenComposite and
leaves SteamVR alone; EDVR identifies which runtime is under it by its exports.
`fix.launch_centre = auto`.

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
