# When something is wrong

Three faults with a known cause and a known answer. Anything else wants
[an issue](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/new/choose)
with the logs attached: run `edvr-installer.exe` and press **Save logs**, which
puts the right session's files into one zip on your Desktop.

**If the fixes that need `openvr_api.dll` do nothing at all**, read
[Headsets and VR runtimes](../README.md#headsets-and-vr-runtimes) first. On
Elite's native Oculus back end that half of the patch is never loaded, and no
install can change that — the log now says so in a `vr runtime:` line.

## The game crashes on launch, on 0.7.1 or earlier

Update to 0.7.2. Before it, EDVR intercepted Direct3D calls by copying an
object's method table and pointing *the object itself* at the copy — which
quietly re-pointed objects ReShade owns and dispatches through, and the game
crashed while EDVR was installing
([#6](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/6)).
It presented as intermittent because EDVR's crash sentinel disables the
Direct3D fixes on the launch after a crash, so it alternated. EDVR now swaps
the individual method pointers where they already live and never touches the
object.

## Everything except the exposure fix stopped working

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

## An earlier version crashed alongside EDHM

The first attempt loaded the other mod during `DllMain`, where Windows holds the
loader lock; loading a DLL that isn't already in memory runs *its* startup code
under that lock, which Windows doesn't support. It now loads the other mod on
the first graphics call instead. Tested with a stand-in proxy that does work in
its own `DllMain` — the exact thing that used to crash — plus the three ways it
can go wrong: a missing name, a non-proxy file, and a setting pointed at EDVR
itself. All three fall back to the system DLL and say so.
