EDVR - an unofficial patch for Elite Dangerous: Odyssey in VR
=============================================================

Six fixes for things that make Odyssey uncomfortable in a headset.


SOMETHING NOT WORKING?

Run edvr-installer.exe and press SAVE LOGS. It puts the last session's logs,
edvr_breadcrumbs.txt, any edvr_FATAL.txt and your edvr.ini into one zip on your
Desktop. Attach that to an issue at
github.com/characterecho-sean/edvr-unofficial-patch/issues

By hand: attach the edvr_logs folder. The log says which fixes installed and
what each one decided, so a report with it attached is usually answerable in one
pass; without it there is very little to go on. If the game will not start at
all, send edvr_breadcrumbs.txt from next to EliteDangerous64.exe instead -- it
is written unbuffered and survives a crash that eats the log.

For setup questions, "is this normal", or just talking about it, there is a
Discord: https://discord.gg/ynkdf6Gdua

EDVR is free and stays free. If it saved you an evening, tips are welcome:
https://ko-fi.com/seancharacterecho


ALREADY RUNNING EDHM OR RESHADE?

Both work alongside EDVR. ReShade needs nothing at all as of 0.7.2 - install it
normally (as dxgi.dll). EDHM installs itself as d3d11.dll, the same name EDVR
uses, so do not just overwrite it: see RUNNING ALONGSIDE OTHER MODS below, which
takes one rename and one setting.


WHAT IT FIXES

One eye going darker than the other near bright lights. Elite works out how
bright the scene is separately for each eye. Near a star or a station floodlight
one eye can see the light and the other cannot, so one eye stops down and the
other does not. Two eyes disagreeing about how bright the world is, which is
uncomfortable and hard to unsee.

Measured on a held view at a star: the eyes differed by about 1.5 stops without
the fix and about 0.4 with it. What remains is the glow around the star in the
eye that can actually see it, which is correct.

On foot not being in 3D - Explorer Cam. On foot Elite draws the world onto a
flat screen and shows that same flat image to both eyes, so nothing has depth.
The external camera renders in proper stereo, so Explorer Cam puts your
viewpoint at your commander's head while you are in that camera and the world
finally has distance in it.

It gives you nothing you could not already do. The external camera is Elite's
own, on your own key. You can move and look around in it, but you cannot
interact with the world: no shooting, scanning, opening a panel, using a
terminal or picking anything up. To do any of that you switch back to first
person exactly as you do now. Explorer Cam changes where the camera is while
you are already in that mode, and nothing else.

It works by replacing one of the camera's presets - COMMANDER RIGHT SHOULDER -
so you cycle to that preset when you want the 3D view. Every other preset keeps
its normal position.

It is off until you tell it your camera key. See EXPLORER CAM below.

The grey haze around the on-foot screen. On foot Elite shows the world on a flat
screen floating in front of you, surrounded by dark grey. On an OLED headset that
grey is lit pixels, so the screen sits in a glowing rectangle instead of in the
dark. This makes it properly black.

The on-foot screen's distance. Fixed by the game, adjustable here. The stock
distance is the default because it is about right.

The one-frame flash when you jump or drop out of supercruise. Once per
transition Elite draws a single frame from the wrong viewpoint - a hard cut to
somewhere else and straight back. On a monitor it is a blink; in a headset the
visual system reads it as the world lurching. EDVR spots that frame and does not
send it to your headset, so SteamVR holds the previous frame for a moment
instead, exactly as it does whenever a game misses a frame.

This fix and Explorer Cam are applied in the second file, openvr_api.dll, which
installs differently from the rest - see the openvr folder in this download,
which has its own instructions. The installer does it for you. An install
without that file is one where this fix can only detect and log, and Explorer
Cam does nothing at all.

The on-foot screen's sharpness. OFF by default - read this bit before turning
it on.

Elite draws that screen at 1920x1080 whatever your headset can do, which is why
text on foot looks soft. This raises it. 4K is a clear improvement, and it
applies to HMD (Cinema Mode) too, which puts the whole game on that same screen.
Set vscreen_res_width and vscreen_res_height in edvr.ini and restart the game.

It is the one fix here that changes the game's code while it runs, and it is off
until you choose otherwise. It does not touch any file on disk, it puts the
original values back when the game closes, and it swaps one number for another
of the same size - so the game runs the same instructions either way. It finds
what to change by recognising its shape rather than by trusting a version
number, and if it does not find exactly what it expects it does nothing and says
so in the log.

It also is not free. At 3840x2160 the game makes over a hundred images of about
32 MB each, plus others that grow with it. If your frame rate drops below your
headset's refresh, step down to 3200x1800 - juddering is worse than soft text.

(Earlier versions of this file said the on-foot screen could not be made
sharper, that it was set in 29 places and had been tried thoroughly. That was
wrong. It is set in ONE place, six times over, and the 29 places were downstream
of it.)


INSTALL

Run edvr-installer.exe, in this folder. It carries both DLLs and edvr.ini
inside it, finds your install (Frontier launcher, Steam or Epic), puts each
file where it goes, and gets the two steps right that manual installs get
wrong: it RENAMES the game's openvr_api.dll rather than overwriting it, and if
another mod already holds the d3d11.dll name it renames that aside and points
EDVR at it so both keep working. It shows you what it is about to do first, and
copies every file it replaces into edvr_backup\.

Run it again after downloading a new version and it updates in place, keeping
the settings you have changed in edvr.ini. It also has Repair, for when another
mod's installer overwrites EDVR's files, and Uninstall, which puts back
everything it renamed.

Windows will say the program is unrecognised, because it is unsigned: More info
-> Run anyway. The release notes carry its SHA-256.

The rest of this section is the same install done by hand.

1. Close Elite Dangerous.

2. Check there is no d3d11.dll already next to EliteDangerous64.exe. If there
   is, you have another mod installed - see RUNNING ALONGSIDE OTHER MODS below
   before going any further, or you will break it.

3. Copy d3d11.dll and edvr.ini into THE FOLDER CONTAINING
   EliteDangerous64.exe. Where that is depends on how you installed the game.
   The ones people have reported:

     Frontier launcher
       ...\Frontier\EDLaunch\Products\elite-dangerous-odyssey-64
       (often under Program Files (x86), and not always on C:)

     Steam
       ...\steamapps\common\Elite Dangerous\Products\elite-dangerous-odyssey-64

     Epic
       ...\Epic Games\EliteDangerous\Products\elite-dangerous-odyssey-64

   If none of those match, search for EliteDangerous64.exe - whatever folder
   holds it is the answer. EDVR writes the folder it loaded from into the first
   lines of its log, so you can check afterwards.

4. Start the game as usual.

Press SCROLL LOCK in game to toggle the brightness fix. Look at a star with one
eye dimmed and press it; the difference is immediate.


UNINSTALL

Run edvr-installer.exe and press Uninstall: it removes EDVR's files, renames
the game's openvr_api.dll back, and puts any mod that was chained behind EDVR
back under its own name. By hand:

Delete d3d11.dll and edvr.ini. That is the whole of it, apart from three things
you can also delete: the edvr_logs folder, edvr_breadcrumbs.txt, and
edvr_FATAL.txt if one is there. All three sit next to EliteDangerous64.exe.

If you also installed openvr_api.dll for the transition flash fix, delete it
from whichever Openvr folder you put it in and rename openvr_api_orig.dll back
to openvr_api.dll.

Frontier's launcher may remove d3d11.dll by itself when it verifies the install.
That is not a problem - it has simply uninstalled EDVR. Copy the file back.


RUNNING ALONGSIDE OTHER MODS

ReShade: nothing to do. Install it the way ReShade tells you to, normally as
dxgi.dll, and both mods' effects apply. (Before 0.7.2 this crashed the game on
launch, sometimes only every other launch. If you are on an older build and see
that, update.)

EDHM installs itself as d3d11.dll, and only one file can have that name. To run
both it and EDVR:

1. Rename the other mod's d3d11.dll to something else, for example
   d3d11_edhm.dll, leaving it in the same folder.

2. Put EDVR's d3d11.dll in its place.

3. In edvr.ini, under [advanced], set:

      real_dll = d3d11_edhm.dll

EDVR then loads that file and passes everything through it, so both mods work.
Anything the other mod does not handle goes to Windows' own d3d11.dll. Restart
the game for it to take effect.

If the name is wrong or the file will not load, EDVR says so in the log and
carries on without it - the game still starts, you just do not get the other
mod.

One catch with EDHM: its uninstaller deletes a file called d3d11.dll, which
after this is EDVR's, not EDHM's. Running it removes EDVR and leaves EDHM's
renamed copy behind. To undo the pair cleanly, delete d3d11.dll and edvr.ini,
rename d3d11_edhm.dll back to d3d11.dll, then use EDHM's uninstaller if you
want EDHM gone too.


EXPLORER CAM

On foot, Elite renders the world to a flat screen and shows that one image to
both eyes. That is why walking around a planet does not feel like VR - there is
no depth in it, because none is being drawn. The external camera does render in
proper stereo, so Explorer Cam moves your viewpoint to your commander's head
while you are in that camera.

It cannot make first person 3D and does not try. The flat screen is flat.

IT REPLACES ONE CAMERA PRESET: COMMANDER RIGHT SHOULDER. While you are on that
preset the camera sits at your commander's head instead of where the preset
normally puts it. That is the whole feature, and it does mean the preset stops
doing its usual job - cycle to it for the 3D view, cycle off it for the normal
framing. Every other preset is untouched and keeps its usual position, so
nothing you already use for screenshots changes. fix.head_offset_view selects
which preset is replaced if you would rather give up a different one.

IT GIVES YOU NO CAPABILITY YOU DO NOT ALREADY HAVE. The external camera is
Elite's own feature, opened with your own binding. You can move and look around
in it, but you cannot act: no shooting, scanning, mining, opening a panel, using
a terminal or picking anything up. To do any of that you switch back to first
person, exactly as you do today.

Explorer Cam changes where the camera is while you are already in that mode, and
nothing else. It does not extend how far you can see, reveal anything the camera
was not already showing, let you act from somewhere you could not act from, or
remove a step anyone else has to take. A player using it and a player without it
can do the same things in the same order with the same clicks. One of them is
looking at it in 3D.

SETTING IT UP

1. Hotkeys: nothing to do. EDVR reads your external-camera and
   next-camera-view keys straight from your Elite key configuration - the
   ON-FOOT camera binding, which Elite keeps separate from the ship's. If
   they are on keyboard keys, you are done: the log's first lines name the
   keys it adopted and the file they came from. Rebind them in Elite, even
   mid-session, and EDVR follows within a few seconds. EDVR only WATCHES
   these keys; it never presses them or interferes with the game receiving
   them.

   They matter because on screen, entering the camera looks identical to
   boarding your ship - the camera key is how EDVR knows which it was. And
   near a planet the game rebuilds its camera data every few seconds, so
   the next-view key's presses carry "which preset am I on" through the
   gaps.

   If your camera is bound only to a controller, bind a keyboard key for
   it in Elite (Options > Controls) for now - EDVR watches the keyboard,
   and controller support is planned.

2. Get on foot, open the camera, and cycle to COMMANDER RIGHT SHOULDER -
   two presses from the view the camera opens on. That is the preset the
   offset replaces, and you cycle to it each time you want the 3D view.
   Every other preset keeps its normal framing; fix.head_offset_view picks
   a different one if you would rather give that one up.

3. Tune these three with the headset on. They reload about once a second, so
   you do not need to restart:

       head_offset_right   = -0.25   + is to your commander's right
       head_offset_up      = 0.25    + is up
       head_offset_forward = 1.25    + is the way your commander faces

   Small numbers, because Commander Right Shoulder already sits close to your
   commander and faces their way; the negative right brings you off the
   shoulder onto the centre line. A preset several metres further back needs
   forward at two to three metres instead. Starting points, not universal
   answers.

COMFORT: these move the viewpoint of a headset you are wearing. Change them a
little at a time. Entering and leaving is a cut rather than a glide, because the
game's own camera change is already a cut.


SETTINGS

Everything is in edvr.ini, next to the game, with a plain description above each
setting. With the file missing you get the defaults, which are what most people
want. The screen's blackness and distance can be changed while the game is
running; the brightness fix and the sharpness fix need a restart.

The sharpness setting ships showing 1920x1080 - Elite's own resolution - rather
than being blank or zero, so you can see what the game does and what you would
be changing it from. As shipped it does nothing.


IF SOMETHING GOES WRONG

Delete d3d11.dll. The game returns to normal immediately. If you installed the
transition flash fix too, also delete the openvr_api.dll you copied in and
rename openvr_api_orig.dll back.

If you report a problem, include everything in edvr_logs\. It is plain text -
open it in Notepad and paste the whole thing. It is a few kilobytes and contains
nothing personal.

If you saw a flash that got through, press PAUSE right after it and then quit.
That writes the last ten seconds of viewpoint history to the log, which shows
whether the frame was spotted and let through or never spotted at all. Those are
different problems and the log is the only thing that tells them apart.


GAME UPDATES

Every fix here finds its target by what that target does or looks like, rather
than by which version of Elite compiled it, so a game update should not break
them. If anything ever stops matching, that fix does nothing, the game renders
normally, and the log says so.

Two fixes work differently, because there is nothing to recognise - what they
watch is data rather than code.

The transition flash fix checks the numbers instead: a viewpoint moves smoothly,
and it requires what it finds to behave that way over 300 rendered frames before
it will act on it. If a game update moves that block, it disables itself and
says so.

Explorer Cam searches for the camera settings by a marker whose position is
recorded in edvr.ini as camera_index_type_offset. A game update will move that
marker. When it does, the search finds nothing, says so in the log, tries again
a few times, and then the offset simply does not engage - it never guesses. If
you know the new value you can set it yourself without waiting for a new build.

Developed against game build 330683 (4.4.0.3).


WHAT IT DOES AND DOES NOT DO

Loads alongside the game as a d3d11.dll proxy, forwarding every call to Windows'
real d3d11.dll.

Most of the fixes never touch the game at all - they change how frames are drawn
from outside it. Two do more, and both ship inert:

  The resolution fix rewrites twelve numbers in the game's code while it runs.
  It is off by default.

  Explorer Cam reads one number from the game's memory - which external camera
  view is showing - and changes the headset position the game is told about, so
  the game moves its own camera. It does nothing at all until you bind your own
  external-camera key, because guessing wrong would move your viewpoint inside
  your cockpit. It never writes to the game's memory.

EDVR also reads event NAMES from the game's journal - the documented file Elite
writes for third-party tools in Saved Games - to know when gameplay has started
and when you step onto your feet, which is where the game resets its camera
view. Nothing else in that file is read or kept, and journal_watch = 0 under
[d3d11] turns it off.

Neither touches the network, your account, or anything the server sees.

The transition flash fix reads one thing from the game: where the renderer is
drawing from. That is camera state, not gameplay state. It never writes to what
it reads, and the only action it can take is to not forward one call to SteamVR.

The sharpness fix is the exception. To raise the on-foot screen's resolution it
rewrites twelve numbers in the game's code, in memory, while it runs: the width
and height that screen is forced to, in the six places the game sets them.
Nothing else is touched. No file on disk is modified, and the original values go
back when the game closes.

None of them touches the network, your account, or anything the server sees.
None reads or changes gameplay state - your position, ship, cargo, credits or
missions. None interacts with anti-cheat, and none tries to hide from anything.

If you would rather nothing here went near the game's code, leave
vscreen_res_width and vscreen_res_height at 1920x1080 and it behaves exactly as
earlier versions did.


ANTIVIRUS

A DLL that sits next to a game executable and intercepts graphics calls looks,
structurally, like something worth flagging. Some scanners will. The source is
published so you can read exactly what it does and build it yourself:

   https://github.com/characterecho-sean/edvr-unofficial-patch


LICENCE

MIT. Not affiliated with, endorsed by, or supported by Frontier Developments plc
or Valve Corporation. Elite Dangerous is a trademark of Frontier Developments
plc.

Modifying the game client is not something Frontier endorses. Use at your own
discretion.

If Frontier asks for this to come down, it comes down. No argument, no mirrors.
