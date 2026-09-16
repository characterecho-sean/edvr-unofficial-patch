# SteamVR names Elite after EDVR's diagnostic: app identity under the OpenXR port

## Status

*Written 2026-09-16. Update whenever this doc changes.*

- **State:** BUILT, NOT FLOWN. SteamVR showed a Frontier-launched Elite as
  "EDVR native stereo diagnostic" with no artwork. Fix: `open()` in
  `src/openxr/native_runtime_host.h` lends the process Elite's Steam app id
  (`SteamAppId=359320`, `src/openxr/steam_identity.h`) for the connect inside
  `xrCreateInstance` and withdraws it as soon as the instance exists; the
  OpenXR `applicationName` is now "Elite Dangerous (EDVR)". Every route was
  measured on the desk against the running SteamVR (journal below), so the
  flight only confirms the game path behaves like the probe.
- **What the user gets:** with Elite in the Steam library, SteamVR files the
  game as `steam.app.359320`: "Elite Dangerous" with Steam's header art,
  exactly as a Steam launch shows. Without it, `steam.app.359320` named "Elite
  Dangerous (EDVR)", no art (measured with an unowned id). A launch that
  already carries `SteamAppId` (Steam's own) is not touched.
- **Ruled out:**
  - `SteamGameId` alone: SteamVR ignores it; only `SteamAppId` is read.
  - Extracting Elite's own icon from `EliteDangerous64.exe`: its icon group
    is 16/32/48 px BMP only, no PNG entry; it would look poor in the
    dashboard and needs an encoder.
  - Editing `C:\Steam\config\appconfig.json` or registering a permanent
    manifest from the installer: needs SteamVR running at install time, and
    leaves an entry behind after uninstall.
  - A colon in the `applicationName` ("Elite: Dangerous"): SteamVR
    lowercases the name into the fallback app key, which names a
    `C:\Steam\config\vrappconfig\<key>.vrappconfig` file.
- **Open (not needed, kept as the next step if wanted):** an own temporary
  manifest gives full control of name and icon for everyone, including "(EDVR)"
  in the name, and was measured to work in-process (journal, run 3). It costs a
  manifest and an icon file on disk plus undocumented reuse of the runtime's
  `IVRClientCore`; the icon would have to be shipped or hotlinked from Steam's
  CDN.
- **Next flight:** any SteamVR flight on this build. Evidence:
  `steam_identity,variable=SteamAppId,source=lent,app_id=359320,withdrawn=1,instance=0`
  in EDVR's native log (absent = `open()` never reached the instance), and in
  `C:\Steam\logs\vrserver.txt` `AppInfoManager.ProcessConnected BEGIN <pid>
  ...\EliteDangerous64.exe 9 steam.app.359320` with no `Creating Builtin
  AppInfo` line for that pid. The dashboard should read "Elite Dangerous" with
  Steam's art.
- **Environment:** SteamVR/OpenXR runtime (`bin\vrclient_x64.dll`, which also
  exports `VRClientCoreFactory`), Pimax via SteamVR, Frontier install launched
  by Frontier's launcher (no `steam_api`, no `SteamAppId`).

## How SteamVR identifies an OpenXR process (vrserver.txt, 2026-09-16)

The game's own connect, before the fix:

```
Creating Builtin AppInfo for ...\EliteDangerous64.exe (VRApplication_OpenXRInstance). , EDVR native stereo diagnostic, 33476
AppInfoManager.ProcessConnected BEGIN 33476 ...\EliteDangerous64.exe 9 system.generated.openxr.edvr native stereo diagnostic.elitedangerous64.exe
```

The format is `Creating Builtin AppInfo for <exe> (<type>). <steam key from
SteamAppId>, <OpenXR applicationName>, <pid>`. Steam's own games never reach
that line: Steam's `steamapps.vrmanifest` lists them as `launch_type: url` with
no binary path, and the process is matched through the `SteamAppId` variable
Steam puts in the environment of everything it launches. A Frontier launch
carries nothing, so SteamVR fabricates the builtin entry and names it after
`applicationName`.

## Desk probe, 2026-09-16 (scratch `probe.exe`, instance only, no headset)

A 200-line C++ probe loaded the pinned loader, created an `XrInstance` against
the running SteamVR, then reached `IVRApplications_007` through
`VRClientCoreFactory` on the `vrclient_x64.dll` the loader had brought in.

| run | what | vrserver.txt key | name / image |
|---|---|---|---|
| 1 | `SteamAppId=SteamGameId=359320` set before loading the loader | `steam.app.359320`, no builtin line | Elite Dangerous / Steam header URL |
| 1b | `SteamAppId` only | `steam.app.359320` | same |
| 1c | `SteamGameId` only | builtin, `system.generated.openxr.<name>.probe.exe` | applicationName / none |
| 1d | `SteamAppId=823500` (not in the library) | builtin, `steam.app.823500` | applicationName / none |
| 2 | no variable; `IdentifyApplication(pid, "steam.app.359320")` after connect | re-keyed: "Unsetting ... because SetApplicationPid came in with a different key" | Elite Dangerous / Steam header URL |
| 3 | no variable; `AddApplicationManifest(temp)` + `IdentifyApplication(pid, "edvr.probe")` | `edvr.probe` | manifest name / `file:///...probe.png` |
| 4 | as 1, variable **unset right after `xrCreateInstance`**, then a 1.5 s session | `steam.app.359320` through `StartSceneApplicationTransitionFromProcess`, the `OpenXRScene` connect (type 10), the transition back and the exit reconnect | Elite Dangerous |

Run 4 is what fixes the design: SteamVR reads the variable once, at the first
connect, and carries the key it assigned through every later transition, so the
loan needs to cover only `xrCreateInstance` and the game runs the rest of its
life with its environment untouched.

Bonus from runs 2 and 3, recorded for the manifest route if it is ever wanted:
inside a process whose OpenXR instance is SteamVR's,
`VRClientCoreFactory("IVRClientCore_003")` returns the core the runtime already
initialised, and `GetGenericInterface("IVRApplications_007")` answers without
any `Init` of our own. The instance kept answering afterwards. Both
`IdentifyApplication` routes log a harmless `[Input] Failed to parse action
manifest : File was empty` in vrserver.txt that the variable route does not.

## Why not "(EDVR)" in the name

The Steam entry's name is Steam's: "Elite Dangerous". The suffix is only
possible with an own manifest (run 3), which is the open item above.
`applicationName` still carries it for the runtimes and fallbacks that show
that string (Pimax's client log, the builtin entry).
