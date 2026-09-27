# When something is wrong

Known faults with known answers for both Flat screen and VR users, plus how to
collect automated diagnostic data for anything new.

If your issue is not listed below, open [an
issue](https://github.com/characterecho-sean/edvr-unofficial-patch/issues/new/choose)
or ask on Discord with your logs attached: run the installer and click **Save
logs**, which gathers the relevant session's logs, breadcrumbs, and settings
into one zip on your Desktop.

For planned installer telemetry features and diagnostic roadmap, see
[diagnostic-reporting-automation-2026-09-26.md](diagnostic-reporting-automation-2026-09-26.md).

---

## Quick Start: Collecting Diagnostic Data (One-Click)

Whether you fly in VR or play in Flat mode, never hunt manually through game
subdirectories for log files. The installer bundles everything automatically:

1. **Close Elite Dangerous completely.** If the game crashed or froze, verify
   in Task Manager (`Ctrl+Shift+Esc`) that `EliteDangerous64.exe` is not hung
   in the background.
2. **Open the installer:**
   - **Flat screen players:** Run `edvr-flat-installer.exe`.
   - **VR players:** Run `edvr-installer.exe`.
3. **Click "Save logs"** on the bottom bar of the installer window.
4. A zip file is written directly to your Desktop:
   `edvr-logs-YYYYMMDD_HHMMSS.zip`.
5. Attach that zip file to your GitHub issue or Discord post.

The bundle automatically includes the latest graphics and OpenXR logs,
`edvr_breadcrumbs.txt`, `edvr_FATAL.txt`, `edvr.ini`, `edvr_install_state.ini`,
`GraphicsConfiguration.xml`, and your local graphics display profiles from
`%LOCALAPPDATA%`.

---

## Flat Screen Troubleshooting

### Flat mode: Shimmer, intense jitter, or edge crawl (Reset Storms)

In flat projection mode with temporal anti-aliasing (TAA/DLSS), EDVR matches
every scene draw call against registered vertex/pixel shader recipes to compute
motion vectors and jitter cancellation.

- **Symptom:** Panning the camera or moving in cockpit/on-foot causes intense
  flicker, geometric buzzing, or trailing ghost smears.
- **Why it happens:** When an unrecognized shader pair executes (such as a rare
  lighting pass or custom mod draw), the runtime cannot safely predict camera
  continuity. To prevent reprojection artifacts, it discards temporal history.
  If this repeats every frame, a "reset storm" occurs.
- **How to capture diagnostic data:**
  1. Go to the in-game scene where the flicker occurs.
  2. Press **`F10`** on your keyboard while looking directly at the artifact.
  3. `F10` arms a bounded 900-frame diagnostic audit: it captures the unknown
     vertex, pixel, and compute shader bytecode, records camera probes, and
     dumps the unrecognized hash pairs to `edvr_gfx_*.log`.
  4. Exit the game, run `edvr-flat-installer.exe`, and click **Save logs**.

### Flat mode: EDHM side-menu haze or blurry angled panels

- **Symptom:** Angled cockpit holographic menus (Navigation on the left,
  System/Status on the right) develop a blurry, glowing haze or smear during
  camera rotation when EDHM (Elite Dangerous HUD Mod) is chained.
- **Why it happens:** EDHM overrides UI pixel shaders to recolor HUD elements.
  The modified shader hashes no longer match the stock signatures in
  `flat_projection_recipes.h`. Sub-pixel jitter is not cancelled for these
  draws, causing TAA to smear them across frames.
- **Verification:**
  - Verify chaining in `edvr.ini`:
    ```ini
    [chain]
    d3d11 = EDHM_x64.dll
    ```
  - While looking at the hazy panel, press `F10` to log the exact replacement
    hashes for inclusion in the recipe dictionary.

---

## VR Troubleshooting

### Checking your active VR runtime

**If the fixes that need `openvr_api.dll` do nothing at all**, read [Headsets
and VR runtimes](../README.md#headsets-and-vr-runtimes) first. On Elite's
native Oculus back end that half of the patch is never loaded, and no install
can change that — the log says so in a `vr runtime:` line.

To see which runtime you are on, look in `edvr_logs\` next to the game after a
session:

| What you find | What it means |
|---|---|
| Native OpenXR startup and runtime name | The bundled loader reached the Windows Active Runtime. |
| Unsupported profile error | This game revision needs an updated EDVR profile. |
| No native startup log | Native startup was not confirmed; attach whatever logs there are. |

### If the game dies a second or two after launch

**As of current versions this fixes itself: update, and it should work with no
`edvr.ini` change.** The launch crash (#20 and #21) happened on rigs where
Windows' `d3d11.dll` re-lays the render context's dispatch table every frame.
EDVR took a *frozen* copy of that table, the copy fell out of step, and the GPU
hung about a second and a half in. `auto` gives those rigs a *live* table that
follows the runtime call by call.

If `edvr_breadcrumbs.txt` ends at `arming d3d11 hooks`, the Direct3D half got
its hooks in and the game died shortly after. EDVR's crash sentinel then turns
those hooks off for the **next** launch by itself, leading to an alternating
pattern of crash, play, crash, play. If a rig still dies that way after
updating, try these settings under `[advanced]` in `edvr.ini`, in this order:

```ini
[advanced]
context_hook_mode = shared
```

`shared` forces the shared table. It is the most conservative mode and composes
cleanly with external wrappers such as ReShade.

```ini
[advanced]
context_hook_mode = live
```

`live` gives the context an EDVR-owned dispatch table that forwards through
small executable stub pages. Try this if `shared` keeps getting overwritten by
third-party hooks.

```ini
[advanced]
d3d11_fixes = 0
```

Turns Direct3D hooks off completely while leaving the VR swapchain and OpenXR
runtime active. A crash that survives this setting is isolated to the VR half
or DXGI boundary.

If you are willing to run one diagnostic flight, add:

```ini
[advanced]
vtable_flip_timeline = 1
```

This logs every change to the Direct3D dispatch table directly into
`edvr_breadcrumbs.txt`. Remember to remove or set it back to `0` afterwards.

### VR failed to start after an EDVR update

EDVR consists of two DLLs that must match: `d3d11.dll` and `openvr_api.dll`. A
mismatched install (one file updated, one old) intentionally aborts startup,
causing Elite to report `VRInitError_Init_Internal`.

**Fix:** Run `edvr-installer.exe` and click **Repair**. It writes both halves
from the single bundled payload. If building from source, run:

```powershell
python tools\install_edvr.py --target steam --verify-only
```

### Everything except the exposure fix stopped working

Look in `edvr_gfx_*.log` for the periodic `vScreen totals:` line. If **`largest
eye-draw count`** is `0` and stays `0`, cockpit UI fixes, panel distance, and
Explorer Cam switch off.

This happens if your per-eye resolution is below 2048 on both axes (e.g. Quest
3 at lower render scales) or if your custom resolution matches the on-foot
panel dimension (`vscreen_res_width = 3840`).

**Workaround:** Set `vscreen_res_width = 2880` or `1920` in `edvr.ini`.

### VR never starts on 0.17.0-rc.1 with EDHM chained

Fixed in versions after rc.1: EDHM's 3Dmigoto previously intercepted
`LoadLibraryExW` for `d3d11.dll` and recursively served the game root copy.
EDVR now takes the copy of Windows' `d3d11.dll` already in memory by its full
path.

---

## Automated Log Inspection & Triage (For Developers & Pilots)

Developers and technical pilots can inspect logs using repository tools in
`tools\`:

```powershell
# Verify installed DLL integrity against current source build
python tools\install_edvr.py --target steam --verify-only

# Check whether a log matches the current git HEAD commit
python tools\edvr_log.py --target steam --expect-build HEAD --version

# Filter log for errors, crashes, and sentinel trip lines
python tools\edvr_log.py --target steam --grep "fatal|error|sentinel|refused|reset"

# View the last 50 lines of the latest graphics log
python tools\edvr_log.py --target steam --tag gfx --tail 50

# Audit eye-texture draw call tallies (census verification)
python tools\edvr_log.py --target steam --tally vh
```
