# One install for flat and VR: Elite's 3D mode picks the profile (design, 2026-10-12)

## Status

- **State (2026-10-13):** phases 1-3 MERGED to main 2769e3e5 (Sean
  approved after the Steam sessions of 14:31-15:22: flat, vr and both
  scopes, live switches, the headset panel, flat XeSS); not yet released. The watch keeps the file's last confirmed
  class apart from the class EDVR runs (a read posts only when the FILE
  changes); rigs run under a pinned LOCALAPPDATA (549e8d91).
- **Goal (Sean, 2026-10-12):**
  - one installer;
  - EDVR runs its flat profile when Elite's 3D mode is off (or anaglyph), and
    its VR profile otherwise;
  - it follows a mid-session switch seamlessly.
- **Decided (Sean, 2026-10-12; section 8):** Q1 switch live; Q2 unknown is
  VR; Q3 no descriptor follows the 3D setting; Q4 no override key (the
  launcher's `/vr` and `/novr` are the game's own, so they count); Q5 one
  installer with a scope prompt; Q6 modes 0-1 flat, 2-6 VR; Q7
  creation-time settings follow at the next launch; Q8 SBS is VR, low
  priority.
- **Reverses:** the two-installer plan (`design-flat-temporal-aa-2026-09-23.md`,
  `design-plugin-architecture-2026-09-30.md`) and the "set once per process"
  profile. **Keeps:** one graphics DLL, separate mono and VR adapters.
- **No hook sees the 3D mode value** (section 2): the switch signal is a
  watch on Settings.xml, prompted by four side-effect hooks (3.2).
- **Design:** scope vs mode (3.1), the switch signal (3.2), one commit point
  at the frame boundary (3.3), what cannot switch live (3.4), the hold keyed
  to the mode (3.5), the scope prompt in one installer (4).
- **Still open for Sean:** none.
- **Hypotheses** (evidence in section 11, the flight journal):
  - H1 open: what a launch with neither switch does at 3D on.
  - H2 HOLDS both ways: `/vr` and `/novr` override the file; applied.
  - H3 REFUTED under a launcher switch; holds for a launch with neither.
  - H4 open: after 3D off the OpenXR session idles at no flat cost.
  - H5 open: the Settings.xml path on every store and on Proton.
  - H6 HOLDS: a 3D apply rewrites the file and the watch sees it.
  - H7 holds so far: no new swap chain at a toggle in B or D.
  - H8 HOLDS: each profile pays under 0.01 ms a frame for the other's hooks.
  - H10 SETTLED: a launch the file did not decide leaves it stale until
    the game's first write, which holds the live mode (f11b7a3e).
  - Regression fixed (7b0358b4): Passthrough stood the window rewrite
    down; 3D on came back 1280x768 windowed and the game saved it.
- **Ruled out while designing:** section 9.
- **Temporary keys:** none. No user-facing key is added.
- **Phase 2 FLOWN OK:** the live switch (e1262071 and its units), no leak
  over 12 switches (10-13), the headset panel while flat (45c51b0f, flown
  OK on Pimax's runtime; panel timing now 10 s text, 1 s black, then zero-layer frames, section 12). Review f519b1d7: three findings fixed (8f13cddf,
  9edf70c7, d410c3dc). Main (XeSS) merged; XeSS in VR in its own doc.
- **Phase 3 BUILT, the installer NOT YET RUN for real** (section 13):
  a7a2c916, 756fb251, 9b642c02, f30817fa, 31b96980, 7ce8a77d, af948bcb.
- **2026-10-14, cold flat start flipped to VR (BUILT, NOT FLOWN; section 14):** a live runtime alone no longer posts Vr; the live
  rise now waits for the game's first eye submit. Flight check: cold flat start (no `/novr`) on a headset rig, F8 opens "flat graphics
  panel open", no `profile switch: flat -> vr`; Epic "Launch in VR" still flips about a second after going live.
- **Next:** the release (notes via reflow_notes.py, credits via
  release_credits.py; the Release build workflow is the CI pass). Not yet
  flown: an Epic install through the GUI, a Frontier launcher launch.
- **Review fixes (2026-10-14):** the flat pad door installs after a VR-first
  menu (input_gate_test); cross-profile cost is sampled only while its
  profile is active, so Passthrough adds none (cross_profile_test); the
  tool's original runtime is found in older receipts (installer.md).

## 1. What exists today

| Piece | VR edition | Flat edition | Where |
|---|---|---|---|
| `d3d11.dll` | same binary | same binary | `build.bat:605-611`, `tools/package_native.py:57-60` |
| `edvr_profile.ini` | `profile = vr` | `profile = flat` | `plan.cpp:221`, `gen_installer_rc.py:360-362` |
| Settings file | `edvr.ini` | `edvr-flat.ini`, which falls back to `edvr.ini` while absent | `config.cpp:142-165`, `ini_name.h` |
| `Openvr\win64\openvr_api.dll` (EDVR's OpenXR runtime), loader, licence, `edvr_openxr.ini` | installed; the game's original is renamed `openvr_api_orig.dll` | not installed; converting from VR restores the original | `plan.cpp:571-662` |
| Installer | `edvr-installer.exe` | `edvr-flat-installer.exe`, the same sources built with `/DEDVR_INSTALLER_FLAT=1` | `build.bat:2563-2620`, `payload.cpp:52-54` |
| Release archives | `edvr-<ver>.zip`, `edvr-installer-<ver>.zip` | `edvr-flat-<ver>.zip`, `edvr-flat-installer-<ver>.zip` | `package.bat:11-14`, `package_native.py:254-317` |

- **When the profile is chosen.** `runtimeProfileInitialize` is the first line
  of `Config::init` (`config.cpp:134`). That runs from `initOnceCallback`
  (`d3d11_proxy.cpp:271`) at the first `D3D11CreateDevice`, or from the fxcfg
  loader hook (`vr_ssaa_hold.cpp:196`). It never runs in DllMain.
- **How it is stored.** The profile is a plain, non-atomic global
  (`runtime_profile.h:8`). About 131 `runtimeFlatProfile()` sites and 18
  `runtimeVrProfile()` sites branch on it.
- **Profile-gated installs at device creation:**
  - flat only: the hull, domain and GS creation hooks
    (`device_hook.cpp:2566-2579`), ResizeBuffers (`:2953-2963`) and
    vscreen's PSSetConstantBuffers hook (`vscreen.cpp:6908`);
  - VR only: the window trampolines (`vr_ssaa_gate.cpp:135`).
- **Mismatches today.**
  - VR profile in a flat game: the VR fixes starve and log so (`vscreen.cpp`
    around 6150-6190).
  - Flat profile in a VR game: nothing detects it. `flat_standdown.h` is temporal
    AA's shape refusal, not this.
- **The OpenXR runtime** (`src/openxr`) has no profile logic. The Oculus route
  is installed in DllMain, profile-blind, and pushes Elite to OpenVR.

## 2. The signal: Elite's 3D mode

- **Where it is saved:**
  `%LOCALAPPDATA%\Frontier Developments\Elite Dangerous\Options\Graphics\Settings.xml`,
  element `<StereoscopicMode>`.
  - The folder is spelled once in `elite_graphics_folder.h`. The DLL reads
    `LOCALAPPDATA` from the environment.
  - The parser is `parseStereoscopicMode` (`vr_ssaa_hold_math.h:15-34`). It
    takes a whole number from 0 to 6 and is rig-tested.
- **Values:**
  - 0 is off.
  - 1 is anaglyph and 2 is side-by-side (Sean, 2026-10-12).
  - 3 was seen at startup in VR flights.
  - 4 shows after a menu 3D-on. Gate H9 is open on it.
  - 5 is HMD Cinema (`fss-scanner.md`).
  - 6 is unnamed.
- **No hook sees the value.** A static-address read was ruled out: the mode
  sits behind virtual calls on heap objects
  (`vr-supersampling-gate-2026-10-10.md`). `vr_ssaa_gate.cpp:165` logs "live
  3D mode not located". What exists, and in which profiles:

| Hook | Profiles | What it sees at a toggle |
|---|---|---|
| Supersampling setter and getter, vtable 0x52E90B0 and 0x52E90B8 (`ui_panel_scale.cpp:444-507`) | both | Fires at toggles ("calls 5-7 passed 1.0 (3D off and on)"), and at ordinary Supersampling edits too |
| fxcfg loader slot 0x52E8368 (`vr_ssaa_hold.cpp:184-281`) | both, from DllMain | Reads Settings.xml itself. It ran about 12 times at startup; no evidence it fires at a toggle |
| Present window observer (`vr_display_observer.cpp:29-52`) | both | At 0 to 4: style 0x94020000 at 3840x2160 became 0x14C80000 at 1280x768, then ResizeBuffers on the same swapchain |
| Window mode request 0x7E9D50 (`vr_window_trampolines.cpp:110-160`) | VR | A kind-0 request at 3D-on, and a fresh Settings.xml read while VR is not live |
| Settings copy 0x281C0D0 (`vr_window_trampolines.cpp:163-169`) | VR | The render context and its Supersampling field; the copy source is unresolved |
| Runtime live flag `edvrQueryNativeRenderSizing().valid`, polled per Present | both | True at VR_Init. False only at VR_Shutdown (`native_runtime_host.h:2543`); no shutdown is recorded at 3D off |

- **Settings.xml timing is unmeasured.** The game writes it on menu-apply.
  The flights disagree on when:
  - the cf11d33c sessions read 3 to 0 and 0 to 4 correctly, 1.5 s after a
    setter call;
  - flight 8a591fba read the file "before the game rewrote it", 1.5 s after a
    setter call.
- **No flight has toggled 3D in the flat profile.**
- **Launcher switches (launch only).**
  - Forum reports say EDLaunch's `/novr` starts the game flat whatever
    Settings.xml says.
  - Epic's "Launch in VR" overrides a `/novr` set in the launcher's settings.
  - Steam lists `/VR`, with its effect unconfirmed.
  - Sources: [Frontier forum, launching without SteamVR](https://forums.frontier.co.uk/threads/is-it-possible-to-launch-ed-without-steam-vr.638753),
    [Frontier forum, VR with the launcher](https://forums.frontier.co.uk/threads/vr-with-elite-dangerous-launcher.638207/latest),
    [Steam discussion](https://steamcommunity.com/app/359320/discussions/0/2968397584533695071).
  - The DLL does not log the game's command line today.

## 3. Runtime design

### 3.1 Scope and mode

- **Scope** is what the user installed, read once from the descriptor:
  `both`, `vr` or `flat`.
  - New descriptor text: `[install]\r\nschema = 2\r\nscope = both\r\n`.
  - An older DLL reads schema 2 as `Invalid`: the fixes are off and chaining
    is kept, which is the right failure.
  - Reading older descriptors (Q3):
    - absent means `both`;
    - schema 1 `profile = vr` means `both`;
    - schema 1 `profile = flat` means `flat`. Such an install has no runtime,
      so VR there runs on the game's own `openvr_api.dll`, untouched.
  - A malformed descriptor stays `Invalid`, which is terminal.
- **Mode** is what the game renders: Flat or Vr. It is live (3.2).
  - Modes 0 and 1 are Flat. Modes 2 to 6 are Vr.
  - Unknown is Vr (Q2). The mode is unknown when the file is absent or
    unreadable, the tag is absent, or the value is out of range.
- **The active profile** is the mode when the scope includes it. Otherwise it
  is a new `Passthrough`: forward only, like `Invalid` but not terminal, so
  the next switch can leave it.
  - `runtimeFlatProfile()` and `runtimeVrProfile()` keep their names and
    become relaxed atomic reads of the active profile.
  - A setter changes it, called only at the commit point (3.3).
  - `runtimeProfileAllowsKey` follows the active profile, as now.
- **The launch decision**, in `runtimeProfileInitialize` (first in
  `Config::init`, not in DllMain), is a pure Windows-free function driven by
  a rig. It takes the descriptor, Settings.xml's text or its absence, and the
  command line. It applies the rules above.
  - `/novr` or `/vr` on the command line overrides the file for this launch.
    This ships only if H2 holds.
  - `parseStereoscopicMode` moves to `src/common`, so the decision, the switch
    watch and the hold share one parser.
- **Log.** The `installation profile:` line (`d3d11_proxy.cpp:289`) becomes,
  for example:
  `profile: flat (scope both; 3D mode 0 from <path>; command line ...)`.
  - It also logs the game's command line once.
  - If the new code never ran, a schema-2 descriptor logs `invalid`, so
    failure cannot pass for success.
  - `edvr_log.py --version` reports the line.

### 3.2 Seeing a switch

Since no hook sees the value, the game's own record of the setting is the
source, read when it is written:

- **The watch.** A change notification on the Options\Graphics folder runs on
  a worker. It could be the journal watcher's worker (`journal_watch.cpp:769`),
  which already watches files off the render thread.
  - On each write to Settings.xml it re-parses the mode.
  - When the mode moves between Flat and Vr, it posts a pending mode.
  - Reading at the write removes the timing problem behind flight 8a591fba's
    stale read (H6).
- **Hints.** These ask the worker to re-check now, and a stat of the file's
  write time backs them up once a second:
  - the Supersampling setter call (both profiles);
  - a window style or size change at Present (both profiles);
  - the runtime's live flag rising.
  - A pending mode must survive one re-read 250 ms later, so a half-written
    file cannot flip the mode. This follows the Bindings poll's two-check rule
    (`device_hook.cpp:1318-1336`).
- **The runtime's live flag rising** also posts Vr when the file still says
  Flat. That covers a VR start that writes no file, such as Epic's "Launch in
  VR".
  - It is not used for the fall: it never falls at 3D off.
- **Log.** Each hint, write, re-read and pending mode is logged with its time.
  Phase 1 ships the watch in this log-only form, so the first flight measures
  the order of events before phase 2 acts on them.

### 3.3 Applying a switch

- **One commit point:** the top of `presentFrameBoundary`
  (`device_hook.cpp:1493`). Both flat Present halves have run there, and no
  draw of the next frame has started. Draw thunks run on any thread, so a
  flip anywhere else splits a frame between pipelines.
- **The sequence on the render thread:**
  1. Tear down the outgoing pipeline while its predicate still answers true.
     `flatRuntimePresent` returns early once not flat (`flat_runtime.cpp:3265`).
  2. Flip the active profile.
  3. Switch the settings file (below).
  4. Run the configure sweep, forced.
  5. Log `profile switch: flat -> vr (Settings.xml mode 4 at <t>; hints ...)`,
     and show a toast.
- **Hooks are installed for every mode in scope, from the first device.**
  - Each hook gates per call on the active profile.
  - This matters most for the flat creation-time capture: input-layout
    private data, hull, domain and GS packets, PS bytecode for
    `FlatOverlayLayer`, and `flatProbeShaders` (`device_hook.cpp:599-757`).
    After a VR-to-flat switch, objects created earlier would otherwise have
    none.
  - The rest install at startup too. That keeps one hook set to reason about:
    - ResizeBuffers (`hookSwapChain` aborts if it fails, `:2964`);
    - vscreen's PSSetConstantBuffers hook;
    - the window trampolines and settings copy;
    - terrain cull cover.
  - Cost is H8. If a hot hook costs VR anything, it installs at the first
    switch into flat and is never removed.
- **Settings file.** A new `Config::switchProfile`:
  - It re-picks the candidates under the lock. The flat profile reads
    `edvr-flat.ini` and falls back to `edvr.ini`; VR reads `edvr.ini`.
  - It snapshots `m_path` for readers. `m_path` is read unlocked today by
    `reloadIfChanged`, the menu writer and `iniName()`.
  - It parses with the `m_lastWrite` shortcut bypassed.
  - It starts from defaults when the file is missing. Today `parse()` on a
    missing file keeps the old map (`config.cpp:215-222`), which would carry
    one profile's values into the other.
  - It bumps `m_generation`.
- **The configure sweep** is `vScreenRefreshConfig`'s roughly 35
  `*Configure(cfg)` calls (`vscreen.cpp:5526`), forced. It is extended with
  the pieces outside it:

| Subsystem | At a switch |
|---|---|
| Flat runtime, temporal, camera inject, flat UI layer | `flatRuntimeResize` (`flat_runtime.cpp:2544`): the full flat teardown, already used at resize and mode-off |
| VR UI layers, panel scale | `releaseLayers`, re-run `uiLayerConfigure`, `uiPanelScaleSetFlatTemporal` |
| DLSS, FSR | bump `g_presetGen` (recreates features). Expose dlaa's file-local `releaseFeatures`; `dlaaShutdown` drops NGX whole. Call `fsr3ReleaseFeatures` |
| Engine velocity | `engineVelocityFlatAbandon`, then `engineVelocityConfigure(false)` and `(true)` |
| Menu | `closeMenu`, `inputGateSetPrivate(false)`, `menuFlatResize`, clear `s.configured`, rebuild pages and the snapshot |
| vscreen | reset `quietPasses`, re-sample `drawGateSet` |
| Supersampling hold | re-set `g_holdAllowed` (set once at the loader today; 3.5) |
| Terrain checkerboard | re-arm `g_startTried`; its reader thread stays |
| Journal watcher | nothing: one worker for both, filtered per call |

### 3.4 What cannot switch live (Q7)

These are patched into the game or the device at creation:

- texture anisotropy and LOD bias (`device_hook.cpp:2606-2608`);
- `fix.vscreen_res_width`, patched into game code (`device_hook.cpp:2894`).
  Images already allocated keep their size.

Decided (Q7): they keep the launch mode's values until the next launch. The
switch's log line names any whose value differs between the two settings
files.

### 3.5 The OpenXR runtime across switches

- **3D off.** Elite is not recorded calling VR_Shutdown, so the session stays
  up and the live flag stays true (H4).
  - The Supersampling hold therefore keys on the active profile being Vr, not
    on the flag alone. Otherwise a VR-to-flat switch leaves flat Supersampling
    held at 1.0. `edvr_log.py:5222` records that as "not chased, by design";
    it must be chased now.
- **3D on after a flat launch.** Whether Elite delay-loads `openvr_api.dll`
  and calls VR_Init at that moment is H1. The runtime needs no profile logic
  either way.
- **A new swapchain at a toggle** would be ignored silently today
  (`hookSwapChain`, `device_hook.cpp:2944`). It gets a log line now (H7).

## 4. Installer and packaging

- **One installer.** `edvr-installer.exe`.
  - Retire `EDVR_INSTALLER_FLAT`, `edvr-flat-installer.exe`,
    `rig_flat_installer` (`build.bat:2603-2621`),
    `gen_installer_rc.py --profile flat` and `package_native.py --profile flat`.
  - The release is `edvr-<ver>.zip` and `edvr-installer-<ver>.zip`.
  - The flat README text folds into `release/README.txt`.
- **The scope prompt (Q5).** The main window asks "Install EDVR for:", with
  three choices:
  - VR and flat (recommended);
  - VR only;
  - Flat only.
  - It defaults to the installed scope, or to both on a fresh install.
  - The command line takes `--scope both|vr|flat`, with the same default, for
    unattended runs.
  - The status line reads the game's 3D mode through the shared parser:
    "Elite's 3D mode is off: EDVR runs its flat fixes."

| Scope | Installs |
|---|---|
| both | graphics DLL, descriptor, runtime with loader, licence and `edvr_openxr.ini`, `edvr.ini`, `edvr-flat.ini` |
| vr | everything `both` installs; flat mode runs `Passthrough` |
| flat | graphics DLL, descriptor, `edvr-flat.ini`; no runtime, so the game's `openvr_api.dll` stays |

- **Changing scope** re-runs the plan.
  - Moving to `flat` removes the runtime through today's VR-to-flat code: the
    ownership proofs at `plan.cpp:249-267`, then the restore at `629-662`.
  - Moving from `flat` installs the runtime.
  - No settings file is ever deleted by a scope change.
  - `--convert-profile`, its MessageBox (`gui.cpp:750-757`) and the profile
    mismatch block (`plan.cpp:234-240`) retire.
- **Payload.** Today's VR payload, plus the generated `edvr-flat.ini`.
- **Two merge bases:** `edvr.ini.base` and `edvr-flat.ini.base`. Today one
  base is shared and overwritten with the incoming edition's ini
  (`plan.h:84-86`, `plan.cpp:791-797`), which leaves one merge two-way.
- **Upgrades.** `installedProfile()` (`plan.cpp:106-114`) learns schema 2, and
  each earlier state upgrades in place:
  - `vr` and legacy installs default to `both`;
  - `flat` installs default to `flat`, with `both` one click away.
- **Settings screen.** A VR/Flat selector, limited to the scope, picks which
  file the list edits. It defaults to the current 3D mode's file. The flat
  list keeps its three rows (`settings.cpp:186-192`).
- **Mirror, bundler and uninstall.**
  - The mirror offers a restore when the scope's files are absent.
  - `editionOf()` reports the scope.
  - `log.dir` is read from both inis, and both folders are swept.
  - `--remove-settings` removes both inis.
  - `docs/installer.md` gets a Status block when this lands.

## 5. Developer tools

- **`install_edvr.py`.**
  - `--profile` becomes `--scope both|vr|flat`, default `both`, writing the
    schema-2 descriptor.
  - The flat path's refusal of VR leftovers (`install_edvr.py:1190-1198`)
    retires.
  - `--ini` writes the scope's settings files, backing up each.
  - `--dry-run` still writes nothing, and the self-test asserts it.
  - Flights pick a mode in the game's menu, not with a pin (Q4).
- **`package_native.py --check-installer`.** One installer embedding both inis
  and the schema-2 descriptor, and no flat artifacts.
- **`edvr_log.py`.**
  - It parses the profile line, the switch lines and the watch's event lines,
    with self-test fixtures.
  - A `--switches` view lists each switch with its hints and timings.

## 6. Tests and build gates

- **A decision rig** (`tools\runtime_profile_test`, added to `build.bat`).
  - Inputs: scope by mode by Settings.xml text, including a BOM, CRLF, an
    absent tag, 7, `-1`, empty and absent.
  - Old descriptors, `/vr` and `/novr`.
  - An older parser must read schema 2 as `Invalid`.
- **A switch rig.**
  - It loads `build\d3d11.dll` on WARP, as `journal_unload_test` does, drives
    frames, and flips the mode through a test-only entry point. It flips flat
    to VR to flat to Passthrough.
  - It asserts one switch line per flip and the teardown lines in order.
  - It asserts that no flat state survives into VR and that the settings file
    follows.
  - It asserts that GPU objects return to their count before the flip: no
    leak per switch.
  - Its control flips with teardown skipped, and must fail.
- **A watch rig.** It covers a write, a half-written file, the 250 ms confirm
  and a hint with no change, against the pure watch logic.
- **`installer_test`.**
  - Each scope fresh.
  - Each old state upgrading.
  - Each scope change, including the runtime removal proofs.
  - The two merge bases.
  - Dry-run writes nothing.
- **Unchanged:** `config_test`'s ini-name resolver and
  `tools\flat_sharpen_test`.

## 7. Flights

Environment: Steam, EDVR's OpenXR runtime via `openvr_api.dll`, Sean's
headset, per-eye size and DLSS version. Run `edvr_log.py --expect-build HEAD`
on every log. The phase-1 instrument flight is one session with three
launches. The phase-1 build switches to Passthrough rather than live.

- **Launch A, mode 0.**
  - Expect `profile: flat (scope both; 3D mode 0 ...)`.
  - The census shows no `openvr_api.dll` while flat (H1, first half).
  - Turn 3D on in the menu, then off, then on. Each toggle logs the setter,
    the window change, the Settings.xml write and the confirm, with times
    (H6).
  - It also logs whether `openvr_api.dll` loads and VR_Init runs at the first
    3D-on (H1, second half), and whether the device or swapchain changes (H7).
- **Launch B, the file now says 4.**
  - Expect `profile: vr (... 3D mode 4)` (H3).
  - Toggle off and on. Expect no VR_Shutdown line, the live flag staying up,
    and flat frame time with the session idle against launch A's (H4).
- **Launch C, the file not 0, EDLaunch with `/novr`.**
  - The logged command line, and whether VR goes live, settle H2.
- **The H8 A/B.** Two VR launches at the same spot, with the always-installed
  hooks against today's VR-only set, read with the CPU census.
- **The Oculus native-SDK supporter's rig**, 3D on: expect the VR profile,
  with the Oculus route still pushing Elite to OpenVR.
- **A Proton user's log:** the profile line names the Settings.xml path (H5).
- **Phase 2's flight** repeats launches A and B with the live switch: each
  toggle's switch line, the flat or VR fixes visibly in effect after it, and
  no leak across ten toggles (GPU memory from the VRAM watch).

## 8. Decisions (Sean, 2026-10-12)

- **Q1, seamless switching.** Decided: switch live (sections 3.2 to 3.5).
  Until phase 2 lands, a mid-session switch goes to Passthrough, with a toast
  to restart.
- **Q2, unknown mode.** Decided: VR.
- **Q3, no descriptor.** Decided: follows the 3D setting (scope `both`).
- **Q4, override.** Decided: none. There are no pins, and flights use the
  game's menu.
- **Q5, release.** Decided: one installer, with the scope prompt of section 4.
  `README.md`'s download section (around lines 59-74) collapses to one. The
  release notes tell flat players the prompt now chooses what they get.
- **Q6, modes.** Decided: 0 and 1 are flat; 2 to 6 are VR.
- **Q7, settings fixed at device creation** (3.4). Decided: they keep the
  launch mode's values until the next launch, and the switch's log line names
  them.
- **Q8, SBS (mode 2).** Decided: VR, for future development and low
  priority. No phase or flight waits on it.
  - SBS renders both eyes to the monitor's swapchain with no OpenXR session.
    The VR fixes are driven by the runtime's eye targets, so today they
    probably starve there and leave SBS as stock Elite.
  - The flat fixes would run temporal AA across two half-width views, which
    is why SBS is not flat.

## 9. Ruled out while designing

- ruled out: "the 3D setting is already hooked" as a source of the value,
  because the four hooks see side effects only, and the live mode was never
  located (`vr_ssaa_gate.cpp:165`).
- ruled out: the runtime's live flag as the 3D-off signal, because it falls
  only at VR_Shutdown, and no shutdown is recorded at a toggle.
- ruled out: re-reading Settings.xml on a fixed delay after a hint, because a
  read 1.5 s after a setter call returned the pre-toggle value in flight
  8a591fba.
- ruled out: flipping the profile anywhere but the frame boundary, because
  draw thunks run on any thread and one frame would split between pipelines.
- ruled out: installing the flat creation-time hooks at the first switch,
  because objects created before then would carry no capture.
- ruled out: deciding in DllMain, because file I/O is not allowed there
  (`oculus_route.h:22-25`).
- ruled out: "no VR frames means flat", because a missing stream proves
  nothing (`design-flat-temporal-aa-2026-09-23.md`).
- ruled out: one merged settings file, because it renames keys or changes their
  meaning by profile, and it loses per-profile tuning.

## 10. Order of work

1. **Launch decision and scope.**
   - The pure decision function, the schema-2 descriptor, and the profile and
     command-line log lines.
   - `Passthrough` on a mid-session switch: the atomic profile, flipped once
     at the commit point after `flatRuntimeResize`.
   - The watch in log-only form, with its hints.
   - `install_edvr.py --scope`, the `edvr_log.py` parsers, and the three
     rigs.
   - Then the instrument flight: launches A to C, and the H8 A/B.
2. **Live switch.** Built on what the flight measured:
   - every hook installed for the scope from the start;
   - `Config::switchProfile`;
   - the extended sweep and the teardown table;
   - the hold keyed on the profile;
   - the switch rig with its leak count.
   - Then phase 2's flight.
3. **Installer and packaging.**
   - One payload, the scope prompt, two merge bases, upgrades, the settings
     selector and the bundler.
   - Update `README.md`, `installer.md`, `manual-install.md`,
     `release/README.txt`, and AGENTS.md's `edvr-flat.ini` bullet.
4. **Release.** Notes via `reflow_notes.py`, and credits via
   `release_credits.py`.

## 11. Flight journal

### 2026-10-12, Steam, phase 1 (Sean)

- 16:03 and 16:05 (build 02a80404): Steam launches `EDLaunch.exe /Steam /VR`, which passes `/vr`. Settings.xml at 0, yet VR went
  live 1.7 s in; EDVR chose flat and dropped to Passthrough at the live rise. The `game command line:` line carried the Frontier
  ServerToken verbatim; it is redacted since 6f3f9362 (exe name and bare switches only).
- B, 16:38 (`/steam /vr`, file 0): `profile: vr ... /vr applied`. 3D off at 16:39:37 took the game out of the headset but
  re-wrote 0 (unchanged), so the watch missed it; 3D on at 16:39:48 wrote 4, seen 80 ms after the setter call and confirmed
  250 ms later (H6). No new swap chain (H7).
- C, 16:42 (`/steam /novr`, file 4): `openvr_api.dll` never loaded, no live rise in 110 s, no HMD mode offered in the menu. EDVR
  chose VR (`/novr` was not applied yet), so F8 opened the VR menu with no compositor. H2 holds both ways; `/novr` applied since
  6fbb4919.
- D, 17:36 (`/steam /vr`, file 0, build 17236b9a): a Supersampling-only apply at 17:37:11 wrote mode 3 (the live mode). 3D off at
  17:37:26.6: eye submits stopped 28.6, file 0 at 31.2, posted flat, Passthrough. 3D on at 17:37:44.27: the window went
  0x94020000 3840x2160 -> 0x14C80000 1280x768 at 44.36, submits resumed 44.6, file 4 at 49.1; DisplaySettings.xml was saved
  1280x768 windowed. Cause: `requestReplacement` forwarded every request once the profile left VR
  (`vr_window_trampolines.cpp:111`); fixed in 7b0358b4.
- ruled out: the file alone as the switch signal after a launch the file did not decide, because it stays stale until the
  game's first write (B missed a real 3D off).
- ruled out: an unrelated apply writing the stale value, because D's Supersampling apply wrote the live mode 3.- E, 18:15 (`/steam /vr`, file 4, build 28a28a4a): 3D off posted flat at 18:16:06 (file before submits this time: the write at
  18:16:06.1, submits stopped 18:16:07.1), Passthrough. 3D on kept the window at 3840x2160 borderless (7b0358b4 works). After 3D
  on the UI was not sharp: Passthrough is a PARTIAL stand-down. Per-call gates stop the upscaler, but the UI layer still built a
  composite output at 2016x2017 (render size, not the 4032x4034 output) at 18:16:21. Phase 2's teardown must release the VR UI
  layers at a switch (3.3's table, `releaseLayers`), and a stand-down must not leave half a pipeline running.- F, 20:18 (`/steam /vr`, file 4, build 44161114, phase 2): the first live switches. 3D off posted flat at 20:19:23.815;
  `profile switch: vr -> flat (... settings edvr-flat.ini; kept until restart: advanced.texture_anisotropy, advanced.texture_lod_bias,
  fix.vscreen_res_width ...)` 19 ms later; the flat F8 panel configured and used. 3D on posted vr at 20:20:12.902;
  `profile switch: flat -> vr (... settings edvr.ini; kept until restart: none)`; the VR menu opened at 20:20:18; Sean: menus sharp
  in VR after 3D on. VRAM 6544 -> 7202 -> 7724 MB across one cycle (one cycle cannot show a leak). The game itself created 672
  textures and 18521 buffers (9.2 GB) at 3D on, with two 150 ms driver stalls and one 366 ms frame: the game's cost, not EDVR's.
  Artifact: `monitor: FREEZE -- 49118.3 ms` at the flat -> vr switch, because perfMonitorFrame runs only from the VR menu tick and
  its clock paused while flat (frames 7770 -> 19442 in those 49 s); fixed in a6ee51a7. The `"off" is not WIDTHxHEIGHT` burst after
  vr -> flat is the flat allow-list reading VR-only keys; a flat launch prints the same lines (13 at 16:03).
## 12. The headset while flat (2026-10-13, Sean: "clear + message panel")

- Problem (flight F and the leak flight): after 3D off the game makes no OpenVR call, the session stays up, and the OpenXR runtime
  (Pimax's, on Sean's rig; not SteamVR) keeps the last submitted frame (the menu), frozen. Elite never calls ClearLastSubmittedFrame (no evidence it does).
- Survey facts: loading frames run from the game's Present callback with the render thread parked during a blocking xrWaitFrame
  (native_module.cpp:40-44, native_runtime_host.h:875-936); in flat that would pace the flat game by the headset. No quad layers
  exist; the runtime owns two eye swapchains and, with `separate_device=1` (the installer's default), its own device and immediate
  context, usable from the owner thread. XR-only calls (xrWaitFrame/BeginFrame/EndFrame with zero layers) are legal from the owner
  thread in either mode. The menu's GDI rasteriser makes no D3D calls (menu_panel.cpp:748-885).
- Design:
  - N1, clear: a `vr profile active` bit in the shared frame-flag block (the intro-recentre pattern), written by the graphics DLL
    at launch and at every switch. While it says not-VR and the game has made no frame call for 250 ms, the runtime's owner thread
    runs its own frames (zero layers), and the Present callback stops routing into the runtime so the flat game never waits on it.
    The game's next WaitGetPoses takes priority and the runtime's frames stop at once.
  - N2, panel (separate device only): the same frames carry a projection layer: both eye images cleared to black with a head-locked
    message rasterised by GDI in the runtime ("Elite is in flat mode. Turn 3D on in Elite's graphics options to return to VR.").
    Borrowed-device mode keeps N1's empty frames. Timing (2026-10-13, Sean): the text shows for 10 s from the stretch's first
    panel frame, the black frames then draw for 1 s more (the compositor holds the last projection frame, so the black has to be
    what it holds), and after that the frames carry no layers (`flat_clear,panel=dark` and `panel=empty`).
- Hypothesis N-H1: the runtime shows its own environment, not the frozen frame, while the app submits zero-layer frames.
  REFUTED on Pimax's runtime (2026-10-13 06:39, build caa65815): the clear ran (1638 and 742 empty frames) and the headset still
  showed the frozen frame. ruled out: zero-layer frames as the fix, because Pimax's compositor keeps the last projection image.
  N2 therefore submits a real image: 45c51b0f (black + message, separate device only). bdeb43cf removed the stereo-table probe:
  ruled out: the per-mode table at ctx+0x3678 as an apply signal, because it read [0,0,0,0,0,0,8192] and never changed across
  four toggles; the file was written 60-310 ms after each setter call, so a game hook would gain at most that.
### 2026-10-13, Steam, phase 2 and the headset clear (Sean)

- Leak flight, 05:26 (build 72b1182f, N1 in): 12 switches, all committed, 6 off/on cycles. VRAM in VR 6582 -> 5977 -> 5967 MB, in flat
  8099 / 7631 / 7977 MB: flat holds about 2 GB more while active and returns it at the switch. ruled out: a per-switch leak, because
  the VR samples fell across the cycles (the earlier +300-400 MB per cycle over 3 cycles was the game streaming).
- Timing over the 12 toggles: Settings.xml written 110-340 ms after the Supersampling setter call; the switch committed 360-600 ms
  after it. Flight D's 4.6 s gap was a one-off. The file watch is a change notification landing within about half a second.
- The clear: `flat_clear,start,quiet_ms=359` about 360 ms after each vr -> flat; 800-4100 empty frames each stretch at about 90 Hz;
  `stop,reason=game_frame` at each 3D on. What the headset showed is Sean's report (N-H1).
- Offline disassembly (build 332841, Steam exe; scripts in the session scratchpad re3d): the mode is parsed into the options block at
  +0x00 (clamped 0..6) and saved from +0x30 of the saved object (fn 0x2871130); the Supersampling vtable (0x52E9000) has a per-mode
  table of 7 counters at ctx+0x3678 (acquire 0x280DED0 `lock inc`, release 0x2866480 `lock dec`, query 0x2845E10). The apply site
  was not found. e18abf76 logs the table read-only each frame (the probe) to see whether it moves at the apply.- H, 11:18 (build d4b5006e, review fixes in): the N2 panel flew: `flat_clear,start,...,panel=1`, `panel=ready,size=1613x745,
  format=29`, 1260 frames, `stop,reason=game_frame` at 3D on. Sean: "3d off message looked good". The frozen frame is fixed on
  Pimax's runtime by a real projection layer (black + message).
## 13. Phase 3 build plan (2026-10-13)

From an inventory of the installer at 11ed0560. Corrected references (section 4's numbers predate the branch): installedProfile
plan.cpp:149-156; the edition-mismatch block 289-295 (GUI dialog gui.cpp:750-758); conversion proofs 305-324 and the runtime
retirement 725-758; flat->VR installs the runtime with no proof gate (667-724); the merge base is one file,
`edvr_install\edvr.ini.base` (state.cpp:19-20), rewritten every install from the payload ini (plan.cpp:894-900), so a flat install
overwrites the VR default; base choice 805-807. EDVR_INSTALLER_FLAT is used only at payload.cpp:55-57 (profile), settings.cpp:186-192
(the flat 3-row filter) and build.bat:2690; everything else branches on payloadInfo().profile at run time. No `--scope`, no scope in
InstallState (state.h:21-61) or Options (plan.h:131-143). The GUI Uninstall never removes settings (gui.cpp:704).

Units, one at a time behind the full build (Haiku implementers, reviewed):
- P3-1 payload: both inis embedded (new id 111 = edvr-flat.ini; gen_installer_rc.py, payload.h/.cpp), the descriptor text chosen by
  the planner for the scope from plan.cpp's canonical texts, not taken from a fixed payload resource.
- P3-2 planner: `scope` in Options and InstallState (old records: profile vr -> both, flat -> flat); install per scope (both = runtime
  + both inis, vr = runtime + edvr.ini, flat = no runtime + edvr-flat.ini); scope changes through the existing conversion proofs
  (to flat) and the runtime install (from flat); no settings file deleted by a scope change; two merge bases (edvr.ini.base,
  edvr-flat.ini.base), each from its own payload ini; the edition-mismatch block and --convert-profile retired. installer_test: each
  scope fresh, each old state upgrading, each scope change incl. the runtime proofs, the two bases, dry-run writes nothing.
- P3-3 GUI and CLI: "Install EDVR for:" (VR and flat (recommended) / VR only / Flat only), default the installed scope or both;
  `--scope both|vr|flat`; a status line from the 3D mode through the shared parser; the settings screen's VR/Flat selector limited to
  the scope, defaulting to the current 3D mode's file, with the flat 3-row filter at run time; the flat edition retired (payload.cpp
  ifdef, build.bat rig_flat_installer, package_native --profile flat, package.bat, the flat README folded into release\README.txt).
- P3-4 mirror, bundler, uninstall: restore offered when the scope's files are absent; editionOf reports the scope; log.dir read from
  both inis and both folders swept; --remove-settings removes both inis and both bases.
- P3-5 docs: README.md download section, docs\installer.md (adds a Status block), docs\manual-install.md, docs\troubleshooting.md,
  release\README.txt, AGENTS.md's edvr-flat.ini bullet and install_edvr's --scope.- H8, cost flight (build 9b671f0d, edvr_gfx_20261013_124833, Steam, 4508x4508 per eye, DLSS): what each profile pays for the other
  in a `both` install, from the cross-profile cost lines (465d74d9). VR for flat's hooks: PSSetConstantBuffers about 270 calls a frame,
  0.6-7.3 us a frame in total (an upper bound: the timing reads dominate), ResizeBuffers 0; flat's creation captures 600 in 20.6 ms
  over the session, at load. Flat for VR's hooks: mode watch 0.08-0.09 us a frame; cull cover, window and settings-copy hooks 0 calls.
  ruled out: a lazy install of PSSetConstantBuffers or deferred flat captures as worth building, because the per-frame cost is under
  0.01 ms of an 11 ms budget and the captures are a one-off 20 ms spread over loading. The cost lines stay as field proof.
  Environment: this flight ran on SteamVR's OpenXR with Pimax's SteamVR driver (`runtime=SteamVR/OpenXR,system=SteamVR/OpenXR :
  aapvr`); Sean tests both runtimes. Per the runtime logs' `headset_key` line: SteamVR for 10-12 16:05-16:39 (B, C) and this flight;
  Pimax OpenXR (Crystal Super) for D, E, F, the leak flights, the 06:40 empty-frame refutation, the XeSS flights and the 11:21 panel
  flight. The panel also flew OK on SteamVR in this session (`panel=ready,size=1803x832,format=29`, 6831 frames, Sean: "the panel
  works"), so it is confirmed on both runtimes.
### 2026-10-13, installer runs (phase 3)

- Real run 1 (Sean, Steam, GUI, before 504c5e20): Uninstall over a dev-tool `both` install with a stale flat record left the
  descriptor, the OpenXR loader files and XeSS behind; the next "Flat only" install refused ("Cannot prove EDVR's VR runtime and the
  game's original"). Fixed in 504c5e20, then corrected after review 2 (provenance before removal: c07ac898; the tool's original
  adopted from its receipt: f6ffb13a). Real run 2 (Sean, Steam, "Flat only" then "VR and flat" then "Flat only"): worked; the "VR and
  flat" merge kept every value of the old edvr.ini (77 keys, 0 changed, 9 user values kept); the report did not mention XeSS though
  it was placed (fixed 78a88b8a).
- Throwaway-folder run (build 456b69f5, an isolated copy of the exe and the stock openvr_api.dll): dev-tool install `both --xess` ->
  GUI update `--scope both` ("Adopting the game's original VR library from the backup the tool made"; openvr_api_orig.dll = stock
  sha) -> `--scope flat` (stock runtime back, loader, licence and edvr_openxr.ini removed, both inis kept) -> `--scope both` (runtime
  back) -> `--uninstall` (d3d11.dll, XeSS trio, DLSS runtime, record and descriptor removed; stock runtime back; both inis kept). Every
  step exit 0, none blocked. %LOCALAPPDATA%\EDVR: only a new edvr-throwaway-test-* mirror; the real mirrors untouched.

## 14. 2026-10-14: a cold flat start flipped to VR (fixed, not flown)

Evidence (reviews\flat-menu-and-real-dll-2026-10-14.md, kept out of git; logs edvr_gfx_20261014_062858 and _063544, Steam, SteamVR
OpenXR present): Elite loads `openvr_api.dll` on a flat start with no `/novr`, EDVR's runtime opens a live session, and
`onLiveRise` posted Vr about 2 s in (`mode watch: live-rise ... -> posted vr`, `profile switch: flat -> vr`) although the game never
submitted a stereo frame (`eye submits last 1 s: 0`, no `eye submits resumed` line). Consequences, all seen: F8 took the VR path
("no compositor consumer"), flat AA stood down, `edvr.ini` was read instead of `edvr-flat.ini`. The design's live rise (3.2) covered
"a VR start that writes no file, such as Epic's Launch in VR"; a cold flat start on a rig with a headset runtime was never flown
(flights A-E were all `/vr` or `/novr`).

Root cause: the live flag means "a session exists" (native_render_settings.cpp), not "the game submits eye frames". The runtime also
sits live while the game is flat on purpose (section 12), so live cannot tell Epic's Launch in VR from a cold flat start.

Fix: `Watch::onLiveRise` (mode_watch_logic.h) over a flat commit only starts a wait; `Watch::onEyeSubmits` posts Vr. The worker
(mode_watch.cpp `checkAwaitedSubmits`) feeds it when `nativeFrameLatchedFrames()` passes the count at the last one-second sample
before the rise, polling every 250 ms while the wait is open. A file post (a confirmed Settings.xml change) ends the wait. A `/vr`
launch runs Vr from the start (a rise is nothing) and a `/novr` launch stays flat until a submit, which `/novr` never makes. Log:
`mode watch: live session without eye submits -- staying flat until the game submits` once per rise; when submits begin,
`mode watch: eye submits began after the live session; the game is a VR game` then the unchanged `live-rise t=N -> posted vr` line
(edvr_log.py parses that one). Cost: Epic's Launch in VR flips up to about 1.25 s later (one-second sample plus the poll).

Tests: tools\mode_watch_test: live with zero submits stays Flat (and the file's 0 reads same, not stale); live then submits flips to Vr;
submits with no rise do nothing; `/vr` and `/novr` launches unchanged; the older live-rise cases drive rise then submits.

ruled out: EDHM's chain as the cause of the dead flat F8, because the 06:35:44 log has no chain line and F8 stayed dead with
`real_dll` commented out.

Open: the second half of that review (`advanced.real_dll` read once from the launch profile's one file) is not part of this fix.
