# Issue 45: the game alternates between dying and starting flat

One reporter: Valve Index on SteamVR, RTX 3090, Steam install, the VR edition
of v0.18.0-rc.2 and rc.3. Evidence is two "Save logs" bundles attached to the
issue (26 and 28 September). Read the Status block first.

## Status

- **State (2026-09-30):** two defects stacked: an unexplained death of the
  armed launch, and a design gap that turns it into "desktop mode". No fix yet.
- **Launch type A, armed (the "crash"):** EDVR arms the d3d11 hooks and the
  game dies within 30 s. 11 of 11 armed launches in the reporter's breadcrumb
  file ended with no `gfx: process exit`, no `gfx: alive` heartbeat and no
  `UNHANDLED` line. The reporter's first launch after install was one, by
  their account; the install record and the tripped first log agree. The two
  armed logs in the bundles stop at the same line, the keyboard-gate capture,
  0.70 to 0.78 s after attach. No `edvr_openxr_*.log` exists for either, so in
  those launches the game never reached `VR_InitInternal`: the death precedes
  the game's first call into the VR half.
- **Launch type T, tripped (the "desktop"):** the sentinel finds the previous
  armed launch unconfirmed and keeps every d3d11 hook off for this launch. The
  VR half needs the graphics owner that `installVScreenFixes` registers,
  finds none, logs `module_startup,graphics_unavailable=80004002` and returns
  `HmdNotFound` to Elite before `xrCreateInstance`. Elite then runs flat and
  SteamVR is never touched. 10 of 10 tripped launches exited cleanly after 31
  to 128 s.
- **The alternation is the sentinel.** A trip clears itself, so the launch
  after a tripped one arms again and dies again: strict A, T, A, T over 21
  logged launches.
- **Why A dies: unknown.** EDVR's crash filter writes `UNHANDLED` for an
  ordinary access violation (Sean's Steam install has six) and is silent on
  stack overflow by design (proxy.cpp:269-272). A fast-fail or an outside kill
  leaves nothing either, so these are silent deaths. Candidates, with what
  separates each (none is evidenced yet):
  1. Stack overflow from hooks chaining into each other (an overlay, EDVR's
     live stubs, the keyboard gate's capture through the Steam overlay's
     table). Application log 1000, code 0xC00000FD.
  2. Fast-fail (CFG, shadow stack, CRT abort): code 0xC0000409 or 0xC00001B2.
     The PR 33 review flagged the unregistered LiveCopy stub pages.
  3. A GPU hang once the hooks run (the #20/#21 class). System log Display
     4101 or LiveKernelEvent 141; EDVR's DEVICE_HUNG line could be lost in the
     250 ms flush gap. Not excluded: neither bundle shows a forced hook mode,
     so the reporter's `shared` trial is unevidenced.
  4. Killed from outside (antivirus behaviour monitor after the CodeHook
     trampolines). No Application Error entry; Windows Security history.
- **Ruled out** (reasons in the evidence section):
  - The sentinel as the cause of the deaths: it only reports them.
  - SteamVR cold start as the cause of the A deaths: no A launch reached the
    VR half. Still open for after A is fixed.
  - An rc.2 to rc.3 regression: same death point, one extra log line.
  - The game build and the config: Sean's healthy sessions run 332841 with the
    same CodeHook sites; neither ini sets a hook mode, ignore_sentinel or
    d3d11_fixes.
  - Steam plus its overlay as a sufficient cause: Sean's Steam install has
    343 clean exits in 351 armed launches.
  - A missing or wrong OpenXR install: the runtime, loader and config all
    checked out in the logs and install records ("Install check").
- **Next:**
  1. Ask the reporter for the Event Viewer entries of one A launch and the
     whole edvr_logs folder (questions at the end).
  2. Build the sentinel flight recorder (section "Proposed build") and fly it
     once on their rig. It dates the death to 100 ms and names the stage.
  3. Decide, Sean: should a tripped launch still start VR. The docs say it
     does and the code says it does not (section "Docs that contradict").
  4. After A is fixed: cold-start SteamVR, which EDVR does not do.

## 2026-09-30: evidence

Logs are local time (UTC+1); edvr_openxr logs are UTC. Builds: rc.2 is
6AB6DC53, linked 2026-09-25 20:40 UTC; rc.3 is 6AB9BF39, linked 2026-09-28
01:13 UTC. Both match the version lines; the edvr_log.py `--expect-build`
check passed for rc.3.

### The five launches in the bundles

| Start | Type | What the files show |
|---|---|---|
| 26 Sep 14:47:01 | T | SENTINEL TRIPPED; VR half `graphics_unavailable`, shutdown 0 ms later |
| 26 Sep 14:48:04 | A | 84 lines, last at +0.78 s; no VR log |
| 26 Sep 14:48:26 | T | same as the first |
| 28 Sep 17:47:04 | T | device +3.5 s, VR half +4.98 s, `graphics_unavailable`, gone in 50 ms |
| 28 Sep 17:48:31 | A | 85 lines, last at +0.70 s; no VR log |

The bundle tool keeps only logs within 180 s of the newest
(logbundle.cpp:22), so earlier launches are not in the zips. The install
record says rc.2 went in at 14:36 on the 26th, ten minutes before the first
log, so at least one armed launch (the reporter's first "crash") is missing.

### The breadcrumb file, all 21 launches (classified by script)

- 11 armed, 10 not armed, strictly alternating, first one armed.
- By tick arithmetic the first two armed launches are from about 25 Sep, a day
  before the rc.2 install record, on a build the reporter did not name. The
  other nine are rc.2 (26 Sep) and rc.3 (28 Sep).
- Armed: 0 have an exit crumb, 0 have a heartbeat, 0 have an UNHANDLED line.
- Not armed: all 10 have `gfx: process exit`, lives 30.8 to 128.4 s.
- The 30 to 128 s lives are presumably the reporter looking at a flat game
  and quitting.

### Where an armed launch dies, against a healthy one

Sean's healthy VR session (Frontier install, v0.18.0-rc.4-26-gc96b91f1,
10:46:53), seconds after attach, against the reporter's armed rc.3 launch:

| Milestone | Healthy | Reporter A |
|---|---|---|
| D3D11 device created | +0.35 | +0.41 |
| Present hook installed | +0.42 | +0.43 |
| first hooked Present | +0.43 | +0.45 |
| keyboard gate captured | +0.65 | +0.70 |
| `vr runtime:` line, game's first VR call | +1.96 | never |
| `native frame: begin #1` | +2.58 | never |

The pace matches to the keyboard gate. The death falls between +0.70 s and the
point where the healthy run first calls the VR half, about +1.95 s. That is the
stretch where the game creates its big resource burst (the healthy log has a
1.5 s long frame there) with every hook armed. The flusher runs every 250 ms
(log.cpp:55), so the silence after the gate line means death before the next
flush or nothing logged, not proof of instant death.

### Why the stock checks are silent

- `gfx: process exit` is written only when DLL_PROCESS_DETACH arrives with
  `reserved` set (d3d11_proxy.cpp:672). TerminateProcess, fast-fail and a
  crash do not deliver it.
- The sentinel confirms after 6 s of presenting (device_hook.cpp:411, 1750).
- `gfx: alive, frame N` is written every 30 s from vscreen.cpp:6380.
- EDVR's filter skips stack overflow (proxy.cpp:269-272).
- Sean's own Steam breadcrumbs, every build since the file began: 351 armed
  launches, 343 exit crumbs, 6 UNHANDLED (all in EliteDangerous64.exe at
  +0x5CBFC8 or +0x5CB322, the input code in
  startup-input-crash-2026-09-14.md), 2 silent; 331 armed launches carry a
  heartbeat. The reporter's rig is 0 of 11.

### Why the T launches are flat

- Elite loads Openvr\win64\openvr_api.dll itself and calls `VR_InitInternal`
  about 1.2 s after device creation (openxr-port.md:142).
- `start()` asks the graphics half for its owner first (native_module.cpp:40);
  `edvrAcquireNativeGraphics` returns `E_NOINTERFACE`, 0x80004002
  (graphics_bridge.cpp:345-346), which is the logged value. `start()` returns
  `VRInitError_Init_HmdNotFound` (native_module.cpp:46-49).
- The owner is registered only inside `installVScreenFixes`
  (vscreen.cpp:6414, 6630), reached only after the sentinel gate and the
  d3d11_fixes gate (device_hook.cpp:2518-2566, 2832). So a trip and
  `d3d11_fixes = 0` fail the same way.
- Nothing launches SteamVR. There is no pass-through to the stock
  openvr_api_orig.dll outside the installer. The sentinel block says "The
  OpenVR half has its own recovery guard" (device_hook.cpp:2561); the legacy
  proxy's other sentinels are gone (compare `git grep 'Sentinel('` at v0.16.2
  and v0.17.0), so that sentence is stale.
- The same tripped launch followed by `graphics_unavailable` was seen on
  Sean's Steam install on 2026-09-14 (startup-input-crash-2026-09-14.md:31-36)
  and read there as a correct refusal; the flat fallback was not noted.

### Docs that contradict the code

These say the VR half keeps running with the d3d11 fixes off:
device_hook.cpp:2504-2507 and 2531-2534, docs/troubleshooting.md:137-139 and
edvr.ini:1689-1694 ("The openvr half keeps running"). By the code above it
does not; the game starts flat. Read, not flown. The troubleshooting page also
offers `d3d11_fixes = 0` as the escape for a rig that dies every launch; on
this rig it would give a stable flat game and nothing else.

### Install check

The reporter says he installed by hand from the zip. The files say the
installer ran for both builds and that the OpenXR parts are in place.

- edvr_install_state.ini is written only by the installer (state.cpp,
  `serializeState`). Both bundles carry a fresh one: rc.2 recorded at 14:36:21
  on 26 Sep (files written 14:36:32), rc.3 at 17:46:27 on 28 Sep (written
  17:46:28, 37 s before the first rc.3 log). edvr.ini has the same stamp. Both
  record all eight components: graphics, profile, ini, openvr, openxr-loader,
  openxr-license, openxr-config, ngx-optional.
- A hand copy of the release zip has d3d11.dll, edvr_profile.ini, edvr.ini,
  openvr/openvr_api.dll, openvr/openxr_loader.dll, the loader notice and
  edvr-installer.exe, and no edvr_openxr.ini (tools/package_native.py:421-423).
  The logs say `module_configuration,source=local`, which needs a valid
  edvr_openxr.ini, a readable openxr_loader.dll and a readable d3d11.dll at the
  configured paths (native_module.cpp:359-369). A hand copy would log
  `source=packaged-default`. So the zip was most likely unzipped and its
  edvr-installer.exe run.
- The game loaded the runtime. `module_init,version=` matches the graphics
  DLL in every tripped launch (rc.2 twice, rc.3 once).
- The `0x80004002` came back from a d3d11.dll found loaded at the configured
  path with the paired exports (native_graphics_client.h:21-66). The pair is
  matched, and only the owner is missing, as the trip explains.
- Hashes: the recorded original openvr_api.dll, 243A818D..., equals the stock
  openvr_api_orig.dll in both of Sean's installs, so the rename was done
  right. The recorded loader, A231A209..., equals Sean's Frontier copy.
- Not checkable from here: the files on his disk today, the loader notice,
  nvngx_dlss.dll, and which OpenXR runtime Windows has active (the README says
  to set it before launching).

### Not determinable from code or these logs

- What ends an armed launch (the candidates above).
- Whether SteamVR's OpenXR runtime starts SteamVR on `xrCreateInstance`. No
  flight or doc covers a cold start; README.md:44-48 says to have the runtime
  up and the headset connected first. `start()` parks Elite's Present thread for
  the whole of `host->start()` (native_module.cpp:66-82). The bounded waits
  inside are a 5 s wait for a Present (native_module.cpp:50) and a 15 s loop
  after open (native_runtime_host.h:678-679); nothing bounds `xrCreateInstance`.
  `xrGetSystem` is one call with no retry (native_runtime_host.h:2444).
- How Elite or the launcher choose VR or flat before any `openvr_api` call.

## Proposed build: the sentinel flight recorder

Not built. Each line below exists to separate the candidates:

- Keep the `.armed` file handle open after `arm()`. Write a fixed-size record
  into it: stage number, tick, frame count. Stages: hooks attached, code hooks
  installed, first Present, keyboard gate, temporal warm, first frame.
- After the first Present, refresh the record every 100 ms for 10 s from a
  tiny thread, then stop. Cost is negligible and bounded.
- The `SENTINEL TRIPPED` line then prints the record: "the previous launch was
  last seen at stage N, T ms after attach, frame F". That arrives in the log
  the reporter already posts from the tripped launch.
- Add `gfx: alive` crumbs at 1, 2, 3 and 5 s.

With the Event Viewer entry this names the cause without a further build.

## Questions for the reporter (a draft, not posted)

1. After a launch where the screen goes black and returns, in Event Viewer
   under Windows Logs, Application: any Error for EliteDangerous64.exe
   (Application Error 1000 or Windows Error Reporting 1001). The faulting
   module, exception code and offset are what is needed.
2. Under Windows Logs, System, near the same minute: Display (nvlddmkm 4101),
   LiveKernelEvent, Kernel-Power.
3. Windows Security, Protection history, and Event Viewer, Applications and
   Services, Microsoft, Windows, Windows Defender, Operational (1116, 1117).
4. Right after one crash launch, before starting the game again: zip the whole
   `edvr_logs` folder and `edvr_breadcrumbs.txt`, and list the folder contents
   (a stale `d3d11_hooks.armed` or `vscreen_auto_eye_width.txt` matters). Also
   list `Openvr\win64` with file sizes; it should hold openvr_api.dll,
   openvr_api_orig.dll, openxr_loader.dll and edvr_openxr.ini.
5. Whether `context_hook_mode = shared` was saved under `[advanced]` before
   the launch; neither bundle shows it.
6. Which overlays run: Steam, NVIDIA App, Afterburner or RTSS, Discord.
7. Optional, one launch: start SteamVR first, headset on, then start Elite.
   It does not bear on the death but it tells whether a cold start is a second
   problem.
