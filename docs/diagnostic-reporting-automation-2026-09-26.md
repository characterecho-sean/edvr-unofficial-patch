# Diagnostic Reporting Automation and One-Click Telemetry

## Status

- State: Proposed / Action Items Defined (2026-09-26).
- Scope: Installer log bundle collection, flat F10 shader sweep, post-crash
  detection, and player-facing triage ergonomics.
- Objective: Eliminate manual file hunting for players reporting crashes and
  rendering anomalies; guarantee that bytecode from flat F10 diagnostic
  captures is automatically packaged.
- Prioritized Action Items:
  - P1: Sweep `edvr_logs\shaders\*.dxbc` in `collectLogs()` (`logbundle.cpp`),
    bounded to 32 recent files and 5 MB total.
  - P1: Concurrency check: warn if `EliteDangerous64.exe` is still running when
    "Save logs" is clicked, preventing locked/empty log captures.
  - P2: Proactive crash banner in `gui.cpp` triggered by `edvr_FATAL.txt` or
    abnormal exit recorded in `edvr_breadcrumbs.txt`.
  - P3: "Copy System Info" button in `gui.cpp` generating sanitized Markdown
    telemetry (GPU, driver, runtime, profile, chain mode) for Discord/GitHub.
  - P4: Toast/sound feedback in `flat_runtime.cpp` when F10 arms and completes.
- Ruled out:
  - Ruled out: Asking players to run PowerShell or Python scripts for triage
    (PowerShell 5.1 escaping and execution policies cause friction; the compiled
    installer executable remains the sole player-facing tool).
  - Ruled out: Unbounded shader dumps into the zip archive (the installer
    builds
    archives in memory; unbounded sweeps risk `std::bad_alloc`).
- Next steps:
  - Implement P1 (`shaders\` sweep and process concurrency warning).
  - Add self-tests in `src/installer/` verifying zip bundling with dummy
    shaders.
  - Update `build.bat` test gates.

## Background & Problem Statement

When players experience a crash or severe visual defect (such as flat mode TAA
reset storms or VR initialization failures), they are often frustrated and
unfamiliar with Elite Dangerous's file system layout. The files needed to
diagnose issues live across multiple disparate directories:
1. `edvr_logs\` next to the game executable (runtime and graphics logs).
2. The game root directory (`edvr_breadcrumbs.txt`, `edvr_FATAL.txt`,
   `edvr.ini`, `edvr_install_state.ini`, `GraphicsConfiguration.xml`).
3. `%LOCALAPPDATA%\Frontier Developments\Elite Dangerous\Options\Graphics\`
   (game settings and display profiles).
4. `edvr_logs\shaders\` (compiled DXBC bytecode dumped by flat F10 captures or
   `shader_dump = 1`).

While `edvr-installer.exe` and `edvr-flat-installer.exe` provide a **Save
logs** button that captures items 1 through 3 into a single desktop zip, item 4
is currently omitted. Furthermore, users often press "Save logs" while the game
is still hanging in Task Manager, resulting in locked files that cannot be
read.

## Prioritized Action Items

### P1: Include Flat Diagnostic Shaders in `collectLogs()`

**Defect**: In flat mode, pressing `F10` arms a 900-frame diagnostic audit that
dumps unrecognized vertex, pixel, and compute shader bytecode to
`edvr_logs\shaders\*.dxbc`. Currently, `collectLogs()` in
`src/installer/logbundle.cpp` only sweeps top-level `edvr_*.log` files.
Consequently, player bug reports lack the actual bytecode required to construct
projection recipes in `flat_projection_recipes.h`.

**Implementation Plan**:
- In `collectLogs()` (`src/installer/logbundle.cpp`), locate `joinPath(logDir,
  L"shaders")`.
- If the directory exists, sweep `*.dxbc` files sorted by `ftLastWriteTime`
  descending.
- Take up to 32 most recent `.dxbc` files, subject to a combined size ceiling
  of 5 MB.
- Store them under a `shaders/` prefix inside the generated zip archive.
- If files exceed the cap, append a note to `bundle.notes`: e.g. "Included 32
  of 48 captured shader blobs (5 MB cap reached)".

### P1: Running Game Concurrency Guard

**Defect**: When Elite crashes, `EliteDangerous64.exe` frequently hangs in the
background while Windows Error Reporting or the audio driver cleans up. If the
player immediately clicks "Save logs", the flight log is still opened
exclusively for writing by the game, causing `writeZip()` to record it in
`skipped` and omit the crucial final lines.

**Implementation Plan**:
- In `src/installer/app.cpp` and `gui.cpp`, query active process snapshots
  (`CreateToolhelp32Snapshot` / `Process32FirstW`) or search for the Elite
  window.
- When `saveLogs()` is triggered, if `EliteDangerous64.exe` is detected:
  - Display a warning dialog or status message in the report pane:
    *"Elite Dangerous is still active in the background. Please close the game first so crash logs can be fully flushed."*
  - Give the user an option to proceed anyway or abort.

### P2: Proactive Post-Crash Detection Banner

**Defect**: A player opening the installer after a crash sees the normal
*Install / Repair / Uninstall* layout. Nothing guides them to click "Save
logs".

**Implementation Plan**:
- During `surveyGame()` in `src/installer/probe.cpp`:
  - Check whether `edvr_FATAL.txt` is present in the game directory.
  - Read the tail of `edvr_breadcrumbs.txt` to detect whether the last recorded
    heartbeat ended with an unhandled exception or abrupt halt without a clean
    `d3d11: device release` entry.
- If an abnormal shutdown is confirmed:
  - In `gui.cpp`, render a highlighted notification card above the button row:
    *"⚠️ A recent game crash was detected. Click 'Save logs' to package debug data for an issue report."*
  - Automatically set default button focus to `kIdCollectLogs`.

### P3: One-Click System Telemetry to Clipboard

**Defect**: Support threads on Discord and GitHub frequently spend days asking
users for their GPU model, Windows build, driver version, and whether they are
running SteamVR, Oculus native, or VDXR.

**Implementation Plan**:
- Add a secondary link button `kIdCopySysInfo` (*"Copy System Info"*) beside
  "Save logs".
- Clicking it queries:
  - EDVR installed profile (`VR` or `Flat`) and commit hash.
  - Active graphics adapter (`DXGI_ADAPTER_DESC` via `D3D11CreateDevice`).
  - Driver version and OS build number.
  - Active OpenXR runtime registry string
    (`HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`).
  - Active chained DLL (`edvr.ini: [chain] d3d11`).
  - Last line of `edvr_breadcrumbs.txt`.
- Format as standard Markdown and place on the Windows Clipboard
  (`OpenClipboard` / `SetClipboardData(CF_UNICODETEXT)`).

### P4: In-Game Feedback for Flat F10 Captures

**Defect**: In flat mode, pressing `F10` silently arms the diagnostic audit.
The player has no visible confirmation that the capture started, which frame it
reached, or when it completed.

**Implementation Plan**:
- When `F10` is pressed in `src/d3d11/flat_runtime.cpp`:
  - Emit an in-game HUD banner or brief log notification (`MessageBeep` or
    on-screen text pass) indicating:
    *"EDVR: Diagnostic capture armed (900 frames). Move camera through artifact."*
  - When the 900-frame audit expires, notify:
    *"EDVR: Capture complete. Use Installer -> 'Save logs' to submit."*

## Flight Test Checklist & Verification

When builds incorporating these features are published, pilots should follow
this verification protocol:

1. **Test Environment**:
   - Clean Odyssey installation on Steam or Frontier.
   - Both `edvr-installer.exe` (VR) and `edvr-flat-installer.exe` (Flat).
2. **Execution Steps**:
   - Flat Mode: Launch game, navigate to a station interior, press `F10`.
   - Confirm in `edvr_gfx_*.log` that `flat unknown projection capture:
     event=armed` appears.
   - Close game, launch `edvr-flat-installer.exe`, and click **Save logs**.
3. **Verification Signals**:
   - Positive: The generated `edvr-logs-*.zip` on the Desktop contains:
     - `edvr_gfx_*.log` and `edvr_openxr_*.log`.
     - `edvr_breadcrumbs.txt` and `edvr.ini`.
     - `shaders/` directory containing captured `.dxbc` files.
     - `game_graphics/` directory containing profile XML files.
   - Negative: Missing `shaders/` folder when F10 was pressed, or skipped log
     files
     due to background process locking.
