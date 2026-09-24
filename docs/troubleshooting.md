# When something is wrong

Six faults with a known cause and a known answer. Anything else wants
[an issue](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/new/choose)
with the logs attached: run `edvr-installer.exe` and press **Save logs**, which
puts the right session's files into one zip on your Desktop.

**If the fixes that need `openvr_api.dll` do nothing at all**, read
[Headsets and VR runtimes](../README.md#headsets-and-vr-runtimes) first. On
Elite's native Oculus back end that half of the patch is never loaded, and no
install can change that — the log now says so in a `vr runtime:` line.

To see which runtime you are on, look in `edvr_logs\` next to the game after a
session:

| What you find | What it means |
|---|---|
| Native OpenXR startup and runtime name | The bundled loader reached the Windows Active Runtime. |
| Unsupported profile error | This game revision needs an updated EDVR profile. |
| No native startup log | Native startup was not confirmed; attach whatever logs there are. |

## If the game dies a second or two after launch

**As of this version this fixes itself: update, and it should just work with no
`edvr.ini` change.** The launch crash
([#20](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/20)
and
[#21](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/21))
happened on rigs where Windows' `d3d11.dll` re-lays the render context's
dispatch table every frame. EDVR took a *frozen* copy of that table, the copy
fell out of step, and the GPU hung about a second and a half in. `auto` now
gives those rigs a *live* table that follows the runtime call by call, so the
default no longer crashes.

If `edvr_breadcrumbs.txt` ends at `arming d3d11 hooks`, the Direct3D half got
its hooks in and the game died shortly after. EDVR's crash sentinel then turns
those hooks off for the **next** launch by itself, so before this fix the usual
pattern was crash, play, crash, play. If a rig still dies that way after
updating, try these three settings under `[advanced]` in `edvr.ini`, in this
order:

```ini
[advanced]
context_hook_mode = shared
```

changes how EDVR attaches to the game's render context. By default, `auto`
checks whose code implements the context and picks for you: a *live* private
copy of the dispatch table (described under `live` below) when the methods are
Windows' own, and the shared table when another mod wraps them. The log line
says which it chose. `shared` forces the shared table. It is the most
conservative mode and composes with a wrapper such as ReShade. If anything
pushes EDVR out of a slot, the log says so by name.

```ini
[advanced]
context_hook_mode = live
```

is what `auto` already gives a rig whose render context is Windows' own, so
most machines run it without any setting. Set it by hand only to return to it
after trying `shared`. It gives the context a dispatch table of EDVR's own, so
nothing else in the process can write the table the game dispatches through,
and each entry reads the game's own entry at the moment of the call. Windows'
`d3d11.dll` re-lays the context's table while the game runs, sometimes onto a
different internal implementation, and a copy taken at startup does not follow
it: that frozen copy is the `private` mode, and it is what issues #20/#21 hung
on. The cost is two extra jumps per Direct3D call, too small to have shown up
in any frame time measured. The risk is that a mod wrapping Direct3D objects
(ReShade as `dxgi.dll`) does not expect its object to be re-pointed, which is
why `auto` gives those rigs `shared`. If `shared` keeps the game alive but the
log then says EDVR's hooks keep being pushed out of the table, `live` is the
mode that cannot be bypassed and never goes out of date. Each `live` hook
forwards through a small executable stub page that EDVR generates (mapped
`PAGE_EXECUTE_READ`: executable, but never also writable) to reach the
runtime's current method for that slot. An antivirus heuristic may weigh that
generated code, and a process that force-enables Control Flow Guard could
refuse a call through one.

```ini
[advanced]
d3d11_fixes = 0
```

turns the Direct3D fixes off for good. Nothing is hooked on the device or on
its render context, so the black void, the panel fixes, the shader replacements
and the anti-aliasing passes are all inert. The `openvr_api.dll` half keeps
working, and so do the swapchain and DXGI hooks that carry the frame boundary
it runs on, so EDVR is still active with this setting. A crash that survives it
is worth reporting, because it is then in one of those hooks or in the VR half.

Please report which of the three you needed, with the log from each, so the
workaround can be turned into a fix.

If you are willing to run one more session purely for the diagnosis, add

```ini
[advanced]
vtable_flip_timeline = 1
```

to whichever of the three you ended up on. It logs every change to the game's
Direct3D function table (what changed, from what to what, at which frame, and
which instruction did it) and writes the first few to `edvr_breadcrumbs.txt`,
which survives a crash that eats the log. That file shows whether the table
changed *before* the crash or *after* it, which none of the reports so far can
settle. Every line that carries a frame number now counts frames the same way,
including the monitor's "LONG FRAME" line, so you can read the order straight
off the file.

On `context_hook_mode = shared` the table changes every frame anyway, since
each change is Windows' own `d3d11.dll` writing its entry back over EDVR's
hook, so the per-change lines stop after the first few thousand and only the
running tally continues. That is expected. EDVR's own writes never appear in
the list, because it unlocks the memory before writing and so raises nothing
for the watch to see. The watch makes every write to the memory the table lives
on take an exception, which costs a few milliseconds a frame. It prints what it
cost and switches itself off if that ever gets serious, though never in the
first ten seconds, which is where the crash is. It is meant for one session:
set it back to 0 afterwards.

## VR failed to start after an EDVR update

EDVR is two files that must come from the same build: the graphics half
(`d3d11.dll`) and the VR half (`openvr_api.dll`). They agree on the size of a
message the VR half sends at startup, and a half-updated install (one file new,
the other old) fails that check on purpose so that neither runs at a size it
did not ask for. Elite then reports `VRInitError_Init_Internal`, and the native
log (`edvr_logs\edvr_openxr_*.log`) carries
`result,native_render_settings_query,-1` with no `openxr_render_size` lines
after it. That `-1` tells you one side is stale but not which.

Run `edvr-installer.exe` and press **Repair**. It writes both halves from the
one package it carries, so they cannot disagree. If you build from source, run
`python tools\install_edvr.py --target <store> --verify-only` (with `steam`,
`frontier`, or the path to the game directory) before every flight of a fresh
build. It compares each installed file's hash with the build and prints `native
verify mismatch:` with the path of the one that differs.

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
  you set `vscreen_res_width` to `3840` (the height follows it at 16:9).
- **Eye textures under 2048 on an axis**, which never qualified at all. This
  is most headsets: a Quest 3 through SteamVR renders about 1832×1920 or
  1728×1824 per eye at ordinary settings, and only clears 2048 on both axes
  near or above its native panel resolution.

From 0.7.3, `openvr_api.dll` reads the size of the texture the game actually
submits and tells the graphics side, so it matches your real eye textures
instead of guessing, and says so in both logs. If the count is still stuck at
0, EDVR now prints a line naming every target size it did see — please attach
it to an issue. As an immediate workaround on any version, set
`vscreen_res_width` to `2880` (short enough that nothing collides) or back to
`1920` to turn the resolution fix off.

## An earlier version crashed alongside EDHM

The first attempt loaded the other mod during `DllMain`, where Windows holds the
loader lock; loading a DLL that isn't already in memory runs *its* startup code
under that lock, which Windows doesn't support. It now loads the other mod on
the first graphics call instead. Tested with a stand-in proxy that does work in
its own `DllMain` — the exact thing that used to crash — plus the three ways it
can go wrong: a missing name, a non-proxy file, and a setting pointed at EDVR
itself. All three fall back to the system DLL and say so.

## VR never starts on 0.17.0-rc.1 with EDHM chained

The game runs flat, and `edvr_openxr_*.log` stops at
`error,D3D11Device,80070005` a few lines after the `size,0,...` and
`size,1,...` lines, then `module_startup,...,result=124`. EDHM's 3Dmigoto
hooks `LoadLibraryExW` and answers a request for Windows' own `d3d11.dll`
with the copy in the game directory, which on an EDVR install is EDVR itself.
The OpenXR half refused that module, as it should, and never created its
device. Fixed in the build after rc.1: the OpenXR half now takes the copy of
Windows' `d3d11.dll` that is already in memory, by its full path, and the log
names the module it got in a `device_module,route=...,path=...` line. Until
you have that build, run 0.16.2 with EDHM, or rc.1 without it.
