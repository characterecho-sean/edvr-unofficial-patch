# The installer

`edvr-installer.exe` — one executable, carrying `d3d11.dll`, `openvr_api.dll`
and `edvr.ini` as resources. It finds the game, installs or updates EDVR,
preserves the settings you have changed, keeps other mods working, and can put
everything back.

This file is why it does what it does. Using it is one paragraph in the
[README](../README.md).

## Why it exists

The manual install is four steps, and two of them are the ones that go wrong.

**The game's `openvr_api.dll` must be renamed, not overwritten.** EDVR's copy of
that file is a proxy: it forwards every call to the original. Overwrite the
original and there is nothing behind the proxy — VR does not start at all, and
the only way back is a file verification in the launcher. The README says this
in bold; it still happens, because "copy the file in" is what every other mod
asks for.

**One `d3d11.dll` name, several mods that want it.** EDHM installs as
`d3d11.dll`. So does anything built on 3Dmigoto. Copying EDVR's file over it
uninstalls the other mod silently — nothing reports an error, the other mod's
effects are simply gone. The fix is a rename plus one line in `edvr.ini`, which
is fine as an instruction and unreliable as a habit.

Both of these are decisions about *which file is which*, made from evidence on
disk. That is a job for a program.

## Finding the game

Nothing is guessed from a fixed path. Each store is asked in its own terms:

| Store | Asked | Then |
|---|---|---|
| Steam | `HKCU\Software\Valve\Steam\SteamPath`, then every library in `steamapps\libraryfolders.vdf` | `steamapps\common\Elite Dangerous\Products\*` |
| Epic | `%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item` (`InstallLocation`) | `<location>\Products\*` |
| Frontier | `%LOCALAPPDATA%\Frontier_Developments`, the uninstall registry (both views), and `Frontier\EDLaunch` on every fixed drive | `<root>\Products\*` |

Every answer is then **confirmed by finding `EliteDangerous64.exe`**, and
anything that fails that test is not offered. A folder the user browses to goes
through the same confirmation. Odyssey products sort first, since that is what
EDVR is for. Three storefronts on one machine is not unusual, so when more than
one install is found the window pre-selects the one that already has EDVR in it.

The `Openvr` folder is found by **file, not by name**: `Openvr\win64`, then
`Openvr`, then the game folder, and whichever holds `openvr_api.dll` *or*
`openvr_api_orig.dll` is the one. Both layouts are in the field, and after an
install the renamed original is the only openvr file with a familiar name.

## Whose file is this?

Every decision below depends on telling EDVR's DLLs from the game's, from
another mod's. That question is answered by **mapping the file read-only and
parsing its export table** — never by loading it. Loading a stranger's
`d3d11.dll` to ask what it is runs that stranger's `DllMain` inside the
installer, and the file we are least sure about is exactly the one we would be
executing.

The discriminator is the same one `tools/gen_exports.py` uses to refuse building
a proxy of a proxy: **an EDVR build exports `edvr_*` symbols** and nothing else
in this world does. It is tested first and unconditionally — an EDVR proxy also
exports everything the DLL it stands in for exports, so testing for
`VR_InitInternal` or `D3D11CreateDevice` first would file every one of our own
installs as the thing it replaced, and the installer would rename our proxy
aside believing it had found the game's original.

`VERSIONINFO` then gives a foreign file a name — ReShade, 3Dmigoto (which is
what EDHM ships), DXVK, OpenComposite — which is both what the report calls it
and what it gets renamed to (`d3d11_edhm.dll`). An unrecognised one becomes
`d3d11_other.dll` rather than being called by a name it might not deserve.

## The two halves

**`d3d11.dll`**, in the game folder:

| What is there | What happens |
|---|---|
| nothing | ours is written |
| ours, same build | left alone (Repair rewrites it anyway) |
| ours, older | backed up, replaced |
| another mod | backed up, renamed `d3d11_<mod>.dll`, ours takes the name, `advanced.real_dll` points at it |
| another mod, and we were installed here before | the same, reported as "it has replaced EDVR's d3d11.dll" |
| another mod, and its own older copy is already parked under that name | the older copy goes to the backup folder, the newcomer takes its place in the chain |
| unreadable (locked, or not a PE) | nothing is touched, and the report says why |

A chain target that has gone away — the other mod was uninstalled properly —
clears `advanced.real_dll` rather than leaving it naming a file that is not
there. That question is asked on every run, including one that does not touch
`d3d11.dll` at all.

**`openvr_api.dll`**, in whichever `Openvr` folder holds it:

| current | `openvr_api_orig.dll` | What happens |
|---|---|---|
| the game's runtime | absent | backed up, renamed to `_orig`, ours written. The first install. |
| the game's runtime | the game's runtime | the game has restored its own file (an update, or a verification): the current one becomes the original, the superseded one goes to the backup folder |
| ours | the game's runtime | ordinary update |
| ours | absent, or ours | **the original is lost.** If one of our own backups holds a genuine runtime it is restored and the install continues; otherwise nothing is written, and the report says to verify the game files in the launcher and run this again |
| absent | the game's runtime | ours is written back |
| anything else | — | left alone, and said so |

An original that turns out to be OpenComposite is chained through normally, with
a note about `advanced.suppress_interfaces`.

## Keeping your settings

An update ships a new `edvr.ini`: new settings, new defaults, rewritten
explanations. Copying it over the installed one throws away every value tuned in
a headset. Leaving the installed one in place means new settings arrive
undocumented and a changed default never reaches anybody. Both are silent, and
the second is worse, because the file still looks right.

So the installer keeps a copy of the shipped default of the version it
installed, in `edvr_install\edvr.ini.base`, and does a **three-way merge**. The
output is the new file, line for line, with your values written back into it:

| Your file vs the version you had | Result |
|---|---|
| you changed a value | your value, in the new file's line |
| you enabled an expert setting (`#fss_res = 0` → `fss_res = 1`) | stays enabled, at your value |
| you never touched it | the new default, comments and all |
| you deleted the line | the new file's line, commented out |
| the setting no longer exists | carried to the end of its section, with a note |
| a key that was never ours | carried verbatim |
| the installer must set it (chaining) | forced, and reported as the installer's doing |

Indentation, the shipped spelling of the key, the spacing around `=` and any
inline comment all survive a rewrite, so a diff against the shipped file stays
readable. The parser follows `src/common/config.cpp` exactly — sections, `#` and
`;`, inline comments needing whitespace in front, last value wins, BOM skipped —
because a merge that disagreed with the game's reader about which value is live
would write a file whose settings are not the ones it reports.

With no base copy (a hand-installed rig meeting the installer for the first
time) it falls back to comparing against the new defaults, which keeps
everything that differs. That over-preserves — a default that moved between
versions reads as an edit — and the report says so rather than pretending it was
a three-way merge. In that mode it does **not** infer deletions: a setting your
older file never had must arrive live, not commented out.

## The settings screen

The second tab is every EDVR setting, with what it does, the value it ships
with, and the range it will accept — edited there instead of in Notepad.
Changes are written straight into `edvr.ini`, keeping its layout and comments,
and the game re-reads that file about once a second, so there is no Apply
button: the change is live by the time the mouse is up.

**The table is generated, not written.** `tools/gen_settings_schema.py` builds
it from the two places the facts already live:

- the **accessor call** in the code gives the type, the declared range and the
  compiled default (`getIntInRange("fix.remlok_line_angle", 46, 20, 60)`);
- the **comment block** above the key in `edvr.ini` gives the explanation, and
  its first sentence becomes the one-line summary in the row.

Writing that down a third time in C++ would be a fourth list to forget to
update — which is the failure `tools/check_config_contract.py` exists to catch
between the other two.

What neither source can know is added by hand, on one annotation line above the
key: what to call the setting in a list, which value is recommended (usually the
default, sometimes a tested pairing like a 0.3 curve with a 0.7 distance), the
choices where the value is a word, and the bounds where they are documented in
prose rather than declared in code.

```ini
# ui: On-foot screen curve | recommended 0.3 | range 0..1
panel_curvature = 0.0

# ui: Sun glare | choices vivid, realistic, stock
sun_glare = vivid

# ui: Scanner: heal the black squares | choices 1=on, 2=both eyes, 0=off
fss_eye_heal = 1

# ui: hidden -- the installer sets this when it chains another mod
real_dll =
```

**Only `[fix]` is shown.** `[advanced]` and `[experimental]` are safety valves
and developer instruments — the log names one when it wants you to change it,
and that is the only way anybody should arrive at them; a list that offers them
invites changing things nobody asked you to change. The remaining sections
(`[hotkey]`, `[log]`, `[openvr]`, `[d3d11]`) are plumbing named after the halves
of EDVR that read them. A `ui:` line outside `[fix]` is a build error, so the
rule cannot drift by accident.

**Every row says when it takes effect.** Most settings are live — EDVR re-reads
`edvr.ini` about once a second — and the four that are not are marked *restart
the game*: `share_exposure`, `transition_flash`, and the two `vscreen_res_*`
sizes. That fact is not a fourth list either: it comes from what the comment
block in `edvr.ini` already says ("Live." or the sentence naming a restart), and
a setting whose prose says neither fails the build. Somebody who changes a
setting and sees nothing happen otherwise has no way to tell a fix that needs a
restart from one that is not working.

**A setting that is live in `edvr.ini` must have one of those lines, or the
build fails.** Uncommenting a key is what promoting a fix to shipped-on looks
like, and a fix that ships on but never appears in the settings window is
invisible to everybody who does not edit ini files — while nothing else in the
build notices, because the game reads it, the log names it, and only the window
that was supposed to expose it is silent. `hidden` is a valid answer, with a
reason; forgetting is not. Commented-out expert settings are exempt: annotate
one and it appears, leave it and it stays where it is.

## Save logs

**Save logs** writes one zip to the Desktop: the most recent session's logs,
`edvr_breadcrumbs.txt`, any `edvr_FATAL.txt`, `edvr.ini` and the install record.
That is the list [Reporting a problem](../README.md#reporting-a-problem) asks
for — four things in three folders, named in a paragraph somebody reads while
annoyed — collected by the program that is already open and already knows which
folder the game is in and where `log.dir` moved the logs to.

*Most recent session* is meant literally. EDVR names its logs
`edvr_<gfx|vr>_YYYYMMDD_HHMMSS.log` and writes two a few seconds apart at each
launch, so the newest stamp and everything within three minutes of it is one
session; older sessions are left out, because an attachment with six launches in
it is harder to read, not more informative. The stamp is taken from the **name**
rather than the write time, since copying a folder, restoring a backup or
unpacking somebody else's zip rewrites write times — all things that happen to a
folder on its way into a bug report.

The zip is written by hand (stored, not deflated) rather than by linking a
compression library or shelling out to PowerShell: logs are small, every tool
opens a stored zip, and the installer stays one file with nothing behind it.
It is the one action that stays available while the game is running, which is
exactly when somebody wants it. `--collect-logs` does the same from the command
line.

## Doing it safely

- **Nothing happens without a yes.** The plan is worked out in full, shown, and
  confirmed before a file moves. Both confirmations default to *No*, because
  Install is the window's default push button and two stray Return presses at a
  window that has just taken focus should not be able to modify a game folder.
- **A stale plan does nothing.** The plan records what each file was when it was
  made; if any of them changed while the confirmation was open — another
  installer ran, a game update landed, a second copy of this window did the job
  — the run is refused before the first step rather than executed against a
  folder it no longer describes.
- **Names that come from `edvr.ini` are checked to be names.**
  `advanced.real_openvr_dll` ends up as a rename destination, and a value with a
  path in it would move the game's runtime somewhere else entirely while the
  report said otherwise. Anything that is not a plain sibling filename falls back
  to the default.
- **Everything replaced is copied into `edvr_backup\<timestamp>\` first.** That
  is also what makes a lost `openvr_api_orig.dll` recoverable later.
- **A failed step rolls the run back.** Each step records its own undo; the
  hazard being defended against is a failure between "rename the game's
  `openvr_api.dll` away" and "write ours", which would leave the folder with no
  `openvr_api.dll` at all. Renames come back; a file that was *replaced* cannot,
  so the rollback never deletes the backups it took and the report distinguishes
  "the folder is as it was" from "what could be undone was undone". Writes go to
  a temp file and are moved into place, so a failure part-way cannot leave a
  truncated DLL where a working one was.
- **Uninstall never leaves the folder without an `openvr_api.dll`.** If the
  game's original is missing it is restored from an EDVR backup, and if there is
  no backup either, ours stays where it is with an explanation — removing it
  would be worse than leaving it.
- **The install record is written last**, so a run interrupted half way leaves
  the previous one, which still describes the folder better than a half-true new
  one.
- **`asInvoker`.** A Steam library on another drive needs no elevation;
  demanding it every time trains people to click through a UAC prompt they did
  not need. When a folder does need it, the installer says so and relaunches
  itself already confirmed.
- **The game must be closed**, and it says which it is rather than failing on a
  locked file halfway through.

## What it does not do

No network access of any kind: it installs what it carries, and "update" means
running the newer installer. No telemetry. No registry writes — the registry is
only read, to find the game. No Add/Remove Programs entry, no shortcuts, no
services, no scheduled tasks. Nothing outside the game folder, `edvr_backup\`
and `edvr_install\`.

## The command line

The window is the product; the command line is what makes it testable and
scriptable.

```
edvr-installer.exe --install [--dir D] [--dry-run]
edvr-installer.exe --repair [--dir D]
edvr-installer.exe --uninstall [--dir D] [--remove-settings]
edvr-installer.exe --collect-logs [--dir D]
edvr-installer.exe --help
```

`--replace-settings` does what it says. Without
`--dir` it finds the installs itself and refuses to guess when there is more
than one. `--dry-run` prints the plan and touches nothing. It is a GUI-subsystem
program, so a command-line run borrows the calling console; run it with
`start /wait` if the interleaved prompt bothers you.

## How it is built and tested

`build.bat` builds it after both DLLs, embedding whatever they are at that
moment. `tools/gen_installer_rc.py` generates the resource script (and the icon)
so a build without the game's `openvr_api.dll` still produces an installer --
one that says in its report that it is a development build carrying half the
patch. `package.bat` refuses to make a release out of it: which halves get
installed is not a choice, and it was one note in a build log away from being
shipped by accident. The linker's own manifest is turned off with
`/MANIFEST:NO` — two `RT_MANIFEST` resources in one image is a program Windows
refuses to start.

`tools/installer_test` is the reason the planner is a pure function of a survey
of the folder. Every case worth getting right is a state of somebody else's
machine — EDHM in the `d3d11.dll` slot, another mod's installer having
overwritten ours, a game update that put the stock `openvr_api.dll` back, an
original lost to a double rename, an `edvr.ini` full of tuned values — and none
of them can be produced to order on the machine doing the build. They are all
written down there instead, along with an end-to-end install, uninstall and
rollback against real files in a scratch folder.

Two guards exist because the failure they catch is invisible: the resource
generator refuses a manifest that is not valid XML, and the build runs the
finished installer with `--help` and fails if it does not start. A bad manifest
links perfectly and dies before `main()`.
