# Installing the two files by hand

The installer does all of this for you, and does the two steps people get wrong
— the `openvr_api.dll` rename, and chaining a mod already holding the
`d3d11.dll` name — without being asked. See
[Install](../README.md#install). This is the same install done yourself, for
anyone who would rather place two files than run a binary.

The second file only matters if Elite is reaching your headset through OpenVR:
[Headsets and VR runtimes](../README.md#headsets-and-vr-runtimes).

Two files. The first enables most of the fixes; the second is needed by the
transition flash fix and Explorer Cam.

## The first file — `d3d11.dll`

1. Close Elite Dangerous.
2. Check there is no `d3d11.dll` already next to `EliteDangerous64.exe`. If
   there is, stop — see [Running alongside other mods](../README.md#running-alongside-other-mods).
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

## The second file — `openvr_api.dll`

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
