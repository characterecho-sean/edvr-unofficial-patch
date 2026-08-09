EDVR - an unofficial patch for Elite Dangerous: Odyssey in VR
=============================================================

Three fixes for things that make Odyssey uncomfortable in a headset.


DO NOT INSTALL THIS IF YOU ALREADY USE EDHM OR RESHADE

They also install themselves as d3d11.dll, only one file can have that name, and
overwriting theirs will break them. Running both crashed the game on startup and
that option has been removed. One or the other, for now.


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

It does NOT make the on-foot screen sharper. That is set in 29 different places
inside the game and could not be changed from outside without breaking the
picture. It was tried thoroughly.


INSTALL

1. Close Elite Dangerous.

2. Check there is no d3d11.dll already next to EliteDangerous64.exe. If there
   is, stop - you have another mod installed and this will break it.

3. Copy d3d11.dll and edvr.ini into the folder containing
   EliteDangerous64.exe, normally:

   %LOCALAPPDATA%\Frontier_Developments\Products\elite-dangerous-odyssey-64

   (Paste that into Explorer's address bar.)

4. Start the game as usual.

Press SCROLL LOCK in game to toggle the brightness fix. Look at a star with one
eye dimmed and press it; the difference is immediate.


UNINSTALL

Delete d3d11.dll and edvr.ini. That is the whole of it, apart from a small
edvr_logs folder you can also delete.

Frontier's launcher may remove d3d11.dll by itself when it verifies the install.
That is not a problem - it has simply uninstalled EDVR. Copy the file back.


SETTINGS

Everything is in edvr.ini, next to the game, with a plain description above each
setting. With the file missing you get the defaults, which are what most people
want. The screen's blackness and distance can be changed while the game is
running; the rest need a restart.


IF SOMETHING GOES WRONG

Delete d3d11.dll. The game returns to normal immediately.

If you report a problem, include edvr_logs\edvr_gfx_*.log. It is plain text -
open it in Notepad and paste the whole thing. It is a few kilobytes and contains
nothing personal.


GAME UPDATES

The brightness fix finds its target by what it does rather than by which version
of Elite compiled it, so a game update should not break it. If anything ever
stops matching, that fix does nothing, the game renders normally, and the log
says so.

Developed against game build 330683 (4.4.0.3).


WHAT IT DOES AND DOES NOT DO

Does: loads alongside the game as a d3d11.dll proxy, forwarding every call to
Windows' real d3d11.dll, and changes three things about how frames are drawn.

Does not: modify, patch or write to the game's memory or any game file. Touch
the network, your account, or anything the server sees. Read or change gameplay
state. Interact with anti-cheat.


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
