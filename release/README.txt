EDVR - an unofficial patch for Elite Dangerous: Odyssey in VR
=============================================================

Five fixes for things that make Odyssey uncomfortable in a headset.


ALREADY RUNNING EDHM OR RESHADE?

They install themselves as d3d11.dll too, and only one file can have that name,
so do not just overwrite theirs. See RUNNING ALONGSIDE OTHER MODS below - it
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

This one needs a second file installed, and it installs differently from the
rest - see the openvr folder in this download, which has its own instructions.
Everything else works without it.

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

1. Close Elite Dangerous.

2. Check there is no d3d11.dll already next to EliteDangerous64.exe. If there
   is, you have another mod installed - see RUNNING ALONGSIDE OTHER MODS below
   before going any further, or you will break it.

3. Copy d3d11.dll and edvr.ini into the folder containing
   EliteDangerous64.exe, normally:

   %LOCALAPPDATA%\Frontier_Developments\Products\elite-dangerous-odyssey-64

   (Paste that into Explorer's address bar.)

4. Start the game as usual.

Press SCROLL LOCK in game to toggle the brightness fix. Look at a star with one
eye dimmed and press it; the difference is immediate.


UNINSTALL

Delete d3d11.dll and edvr.ini. That is the whole of it, apart from three things
you can also delete: the edvr_logs folder, edvr_breadcrumbs.txt, and
edvr_FATAL.txt if one is there. All three sit next to EliteDangerous64.exe.

If you also installed openvr_api.dll for the transition flash fix, delete it
from Openvr\win64 and rename openvr_api_orig.dll back to openvr_api.dll.

Frontier's launcher may remove d3d11.dll by itself when it verifies the install.
That is not a problem - it has simply uninstalled EDVR. Copy the file back.


RUNNING ALONGSIDE OTHER MODS

EDHM and ReShade also install themselves as d3d11.dll, and only one file can
have that name. To run both:

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

The transition flash fix is the one exception, because there is nothing to
recognise - the viewpoint it watches is a block of numbers rather than code. So
it checks the numbers instead: a viewpoint moves smoothly, and it requires what
it finds to behave that way over 300 rendered frames before it will act on it.
If a game update moves that block, it disables itself and says so.

Developed against game build 330683 (4.4.0.3).


WHAT IT DOES AND DOES NOT DO

Loads alongside the game as a d3d11.dll proxy, forwarding every call to Windows'
real d3d11.dll.

All but one of the fixes never touch the game at all. They change how frames are
drawn from outside it. With the sharpness fix left off - which is how it ships -
nothing in the game's memory is written.

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
