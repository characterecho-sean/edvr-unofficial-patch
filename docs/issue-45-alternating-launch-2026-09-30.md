# Issue 45: the game alternates between dying and starting flat

One reporter: Valve Index on SteamVR, RTX 3090, Steam install, the VR edition
of v0.18.0-rc.2 and rc.3. Evidence is two "Save logs" bundles attached to the
issue (26 and 28 September). Read the Status block first.

## Status

- **State (2026-10-02):** Root cause isolated and fix implemented.
  `factoryCreateDevice` queried `GetCapabilities` on every DirectInput device,
  crashing unconfigured third-party force-feedback drivers (G29 / jerry_forcefeedback_x64).
  Fixed by filtering on system keyboard GUIDs before capture and adding
  `advanced.input_gate` toggle.
- **Launch type A, armed (the "crash"):** EDVR arms the d3d11 hooks and the
  game ends within 30 s. 14 of 14 armed launches in the reporter's breadcrumb
  file (rc.2, rc.3, v0.18.0 and one earlier build) have no `gfx: process exit`,
  no heartbeat and no `UNHANDLED` line. The armed logs in the bundles stop in
  the game's first long frame and no `edvr_openxr_*.log` exists for them, so
  the game never reached `VR_InitInternal`.
- **What the v0.18.0 logs add:** after its first Present the render thread
  spends 1.0 s and more in DirectInput device creation: a WMI query, device
  enumeration (CfgMgr32, HID, SetupAPI) and a third-party force-feedback
  driver (`jerry_forcefeedback_x64`; the reporter guesses his G29 wheel), all
  through the Steam overlay's hooks. No later Present or log line follows.
  The sampler checks 14 frames for EDVR code: the callers above are unseen.
- **Launch type T, tripped (the "desktop"):** the sentinel finds the previous
  armed launch unconfirmed and keeps every d3d11 hook off for this launch. The
  VR half needs the graphics owner that `installVScreenFixes` registers,
  finds none, logs `module_startup,graphics_unavailable=80004002` and returns
  `HmdNotFound` to Elite before `xrCreateInstance`. Elite then runs flat and
  SteamVR is never touched. 13 of 13 tripped launches exited cleanly after 31
  to 199 s.
- **The alternation is the sentinel.** A trip clears itself, so the launch
  after a tripped one arms again and dies again: strict A, T, A, T over 27
  logged launches.
- **Fix:** `isKeyboardGuid(guid)` check added to `factoryCreateDevice` so
  non-keyboard devices (force-feedback wheels, pedals, joysticks) are never
  probed for capabilities. `advanced.input_gate` override added.
- **Ruled out** (reasons in the evidence sections):
  - The sentinel as the cause of the deaths: it only reports them.
  - SteamVR cold start as the cause of the A deaths: no A launch reached the
    VR half. Still open for after A is fixed.
  - An rc.2, rc.3 or v0.18.0 regression: the same death point in all three.
  - The game build (Sean's healthy runs use 332841) and the config (no hook
    mode, ignore_sentinel or d3d11_fixes set).
  - Steam plus its overlay as a sufficient cause: Sean's Steam install has
    343 clean exits in 351 armed launches.
  - A missing or wrong OpenXR install: the runtime, loader and config all
    checked out in the logs ("Install check").
- **Next:** User test flight with the fix build.


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

## 2026-10-02: third bundle, the v0.18.0 release build

The reporter reinstalled from `edvr-installer-0.18.0.zip` and says the
symptoms are the same ("previously I'd been using the zip with individual
files in it"). Bundle edvr-logs-20261002-090639.zip, 300,953 bytes, from the
issue comment. All three gfx logs read `v0.18.0 (build 6ABED11D)`, linked
2026-10-01 21:31 UTC, the tag's release build; `edvr_log.py --expect-build
v0.18.0` passed. Local time is UTC+1.

### The three launches

| Start | Type | What the files show |
|---|---|---|
| 2 Oct 09:03:19 | A | 152 lines, last at +1.68 s, a 1005 ms stall sample |
| 2 Oct 09:03:57 | T | SENTINEL TRIPPED; VR half at +1.75 s, `graphics_unavailable`, gone in 0 ms |
| 2 Oct 09:05:16 | A | 149 lines, last at +1.40 s, a 1003 ms stall sample |

The breadcrumb file now holds 27 launches: 14 armed, none with an exit crumb,
heartbeat or UNHANDLED line; 13 tripped, all with an exit crumb, lives 31 to
199 s. Strictly alternating, first and last armed.

### What the stall sampler caught

The sampler (stall_sampler.h at v0.18.0; `advanced.freeze_location`) stops the
render thread briefly at 150, 500 and 1000 ms without a Present, copies its
stack and walks at most 14 frames (`kMaxFrames`, line 162). In both armed
launches every sample says "last Present returned in frame 1".

| Sample | A at 09:03:19 | A at 09:05:16 |
|---|---|---|
| 150 ms | WMI wait (combase, fastprox) under the Steam overlay, from EliteDangerous64.exe+0x5C952D and +0x5C74D3 | the same stack |
| 500 ms | CfgMgr32 enumeration under DINPUT8, under the overlay | HID.DLL under the overlay under DINPUT8, then jerry_forcefeedback_x64 |
| 1000 ms | wait in DINPUT8 under jerry_forcefeedback_x64, which called back through the overlay into DINPUT8 | SetupAPI and CfgMgr32 under the overlay under DINPUT8 |

- The 150 ms sample reaches the game's own frames. The other four stop at the
  14-frame cap inside DirectInput, so what called DirectInput is not on
  record. "EDVR code on the stack: no" holds for those 14 frames only; EDVR's
  `CreateDevice` thunk, which would sit above them, was never examined.
- The keyboard-gate capture line falls inside the first stall (+1.02 s and
  +0.65 s). The last sample lands at +1.68 s and +1.40 s. Nothing follows: no
  fourth sample (the sampler takes three per episode, so a hung thread gives
  exactly this), no later Present, no `LONG FRAME` line, no openxr log. At its
  last sample the thread was still in DirectInput, which makes a GPU hang of
  EDVR's passes unlikely: no second frame had begun.
- The tripped launch passed the same stretch: its VR half loaded at +1.75 s,
  while the armed launches were still inside DirectInput at +1.4 to +1.7 s.
- A second-long first frame is normal. Sean's seven sampler logs on the
  Frontier install carry 12 startup samples at 1000 ms. What differs is where
  the time goes: his startup stalls are in COM, the game, NVAPI and the VR
  runtime, and 7 of 53 lines carry any DirectInput, HID, device-enumeration,
  overlay or WMI frame.

### The keyboard gate, read at v0.18.0

input_gate.cpp is identical in rc.3 and v0.18.0. The capture design (commit
188ffbdf) has shipped since v0.15.0, so v0.16.2 has it too.

- Always on: `inputGateInstallEarly` (DllMain) swaps the exe's
  `DirectInput8Create` import for `hookDirectInput8Create` (lines 1017-1023,
  595-606).
- On the game's `DirectInput8Create`: `captureFactory` patches slot 3
  (`CreateDevice`) of the returned object's vtable in place and `AddRef`s the
  object (566-593).
- On every `CreateDevice`: `factoryCreateDevice` calls the original, then
  `captureKeyboard`, whose `isKeyboard` calls `GetCapabilities` through the
  new device's vtable (554-563, 500-503, 235-244). The keyboard gets its
  `GetDeviceState` and `GetDeviceData` patched and one more `AddRef`
  (516-547). Every other device, the wheel included, is probed and left alone.
- The trip path calls `inputGateShutdown` (device_hook.cpp:2608 at v0.18.0),
  which removes the import hook (987-1015). That happens at device creation,
  +0.37 s in the tripped launch, before the game's DirectInput calls. A
  tripped launch has no gate in this stretch. The gate is the one armed piece
  that calls into DirectInput.
- Why it could matter, as a mechanism and not as evidence: `GetCapabilities`
  on a force-feedback device asks the vendor driver for its version and
  timing. The game would make that call later, after configuring the device,
  if at all. The driver's own `DirectInput8Create` is in the samples.

### The reporter's answers, and an oddity

- Asked about the DLL, he says: perhaps his G29 racing wheel; he usually runs
  Thrustmaster TARGET as a HID remapper but has tried without it, no change.
  The TARGET test does not bear on the gate, since the wheel stayed plugged
  in. A web search could not place `jerry_forcefeedback_x64.dll`, so the wheel
  is his guess.
- There is no `edvr_install_state.ini` in this bundle, although edvr.ini was
  rewritten at 09:02:40, 39 s before the first launch. The bundler adds the
  record only if `edvr_install\state.ini` exists (logbundle.cpp:426-429) and
  the installer writes it as the last step of its plan (plan.cpp:799-804). An
  interrupted install or a removed folder would look like this. The runtime
  log says `source=local` again, so the OpenXR files are in place.

## Proposed build: gate switch, deeper stacks, flight recorder

Not built. One flight on the reporter's rig should separate the candidates:

- `advanced.input_gate = off`: `hookDirectInput8Create` forwards without
  capturing and `inputGateInstall` does nothing. The key is read at device
  creation, before the game's first DirectInput call. If an armed launch then
  survives, the gate is the cause. The menu loses its private keyboard.
- A second switch value that keeps the gate but replaces the `GetCapabilities`
  probe with a test on the GUID given to `CreateDevice` (the system keyboard
  GUIDs), falling back to the existing dummy-device path. If armed launches
  then survive, the probe is the cause; if they still die, look at the extra
  references and the `CreateDevice` patch. Neither ships without the flight.
- Raise the sampler's walk past 14 frames for the first episode, print the
  outermost frames and say whether EDVR's `CreateDevice` thunk is among them.
  Add samples at 2000 and 5000 ms.
- The sentinel flight recorder: keep the `.armed` file open after `arm()`;
  write stage, tick and frame count into it; refresh every 100 ms for 10 s
  from a tiny thread; print it in the `SENTINEL TRIPPED` line. Add
  `gfx: alive` crumbs at 1, 2, 3 and 5 s.

With the reporter's answers this names the cause without a further build.

## Questions for the reporter (a draft, not posted)

1. Unplug the G29 and any other force-feedback or racing device, keep the
   headset and the HOTAS, and launch Elite from Steam. Does the first launch
   reach VR?
2. Separately: Steam, Library, Elite Dangerous, Properties, General, untick
   "Enable the Steam Overlay while in-game". Launch again.
3. On a launch where the screen goes black: does the Elite window stay up and
   go "Not responding", or vanish by itself, and after how many seconds? Is
   EliteDangerous64.exe still on Task Manager's Details tab? If it is, right
   click it there, choose "Create memory dump file", and either send it or
   open it in WinDbg or Visual Studio and send the stacks of all threads.
4. Where is `jerry_forcefeedback_x64.dll`? Search System32, Program Files and
   Program Files (x86), then Properties, Details: product name and company.
5. In Event Viewer, Windows Logs, Application: any Error for
   EliteDangerous64.exe (Application Error 1000 or 1001), with the faulting
   module, exception code and offset. Under System, near the same minute:
   Display (nvlddmkm 4101), LiveKernelEvent, Kernel-Power.
6. Windows Security, Protection history, and Event Viewer, Applications and
   Services, Microsoft, Windows, Windows Defender, Operational (1116, 1117).
7. Right after one crash launch, before starting the game again: zip the
   whole `edvr_logs` folder and `edvr_breadcrumbs.txt`, and list the contents
   of `edvr_install`, `edvr_logs` and `Openvr\win64` with file sizes. Say
   whether the installer ended on its "done" page.
8. Was `context_hook_mode = shared` saved under `[advanced]` before a launch?
   No bundle shows it.
9. Which overlays run: Steam, NVIDIA App, Afterburner or RTSS, Discord?
10. Optional, one launch each: SteamVR started first, headset on; and the
    public v0.16.2, which has the same keyboard gate and a different VR half.

## 2026-10-02: Fix implemented

1. In `src/d3d11/input_gate.cpp`, `factoryCreateDevice` now checks `isKeyboardGuid(guid)`
   against `kGuidSysKeyboard`, `kGuidSysKeyboardEm`, and `kGuidSysKeyboardEm2`. Non-keyboard
   devices (such as the Logitech G29 force-feedback steering wheel using `jerry_forcefeedback_x64.dll`)
   are never passed to `captureKeyboard` or probed with `GetCapabilities`.
2. Added `advanced.input_gate` toggle (default true). When off, DirectInput creation
   hooking is bypassed completely.
3. Extended `tools/input_gate_test/input_gate_test.cpp` to verify non-keyboard GUID bypass.
