# Installing native OpenXR by hand

The release installer validates the game revision, preserves settings and
handles graphics-mod chaining. Use it for upgrades or an uncertain existing
installation. These steps describe a fresh manual install of the complete
native package.

1. Close Elite Dangerous.
2. Find the folder containing `EliteDangerous64.exe`. Typical locations are
   `Products\elite-dangerous-odyssey-64` under the Frontier launcher, Steam's
   `Elite Dangerous` directory, or the Epic installation.
3. If another mod already owns `d3d11.dll`, preserve it and configure the
   `advanced.real_dll` chain as described in [Running alongside EDHM by
   hand](#running-alongside-edhm-by-hand) below. Never overwrite another mod's
   only DLL.
4. Place the release's `d3d11.dll` beside `EliteDangerous64.exe`. Copy
   `edvr.ini` there only if you do not already have one; otherwise keep your
   existing settings.
5. Preserve the game's original `Openvr\win64\openvr_api.dll` as
   `openvr_api_orig.dll`. If that original already exists, use the installer
   rather than renaming a previously installed EDVR DLL over it. The original
   is for uninstall; native EDVR never loads it.
6. Copy all three files from the release's `openvr` directory into
   `Openvr\win64`: `openvr_api.dll`, `openxr_loader.dll`, and
   `OPENXR-LOADER-LICENSE.txt`.
7. If supplied and wanted for DLSS/DLAA, place `nvngx_dlss.dll` and its NVIDIA
   license beside the game. Preserve a deliberately installed newer DLSS
   version.
8. Choose your headset software's OpenXR runtime as Windows' active runtime,
   connect the headset, and launch Elite normally.

The native module uses its sibling loader and the game's `d3d11.dll` when
`edvr_openxr.ini` is absent. No manual configuration is needed for the layout
above. A present but invalid startup configuration is an error. The installer
writes a valid configuration automatically and supports other detected OpenVR
directory layouts.

SteamVR may be the selected OpenXR runtime. When another runtime is selected,
the package does not require SteamVR or OpenComposite. There is no legacy
OpenVR/LibOVR backend fallback. Logs go into `edvr_logs` beside the game.

For uninstall, close Elite and use the installer, or remove the EDVR files and
restore the original `openvr_api.dll` and any graphics-mod chain. Restore the
original before launching the unmodified game. Preserve your `edvr.ini` if you
intend to reinstall.

## Running alongside EDHM by hand

EDHM also installs as `d3d11.dll`. To run both:

1. Rename EDHM's `d3d11.dll` (say, to `d3d11_edhm.dll`) and leave it where it
   is.
2. Put EDVR's `d3d11.dll` in its place.
3. In `edvr.ini`, under `[advanced]`, set `real_dll = d3d11_edhm.dll`.

EDVR passes everything through EDHM, and anything EDHM doesn't handle falls
through to Windows' own `d3d11.dll`. Restart the game for this to take effect.
If the name is wrong or the file won't load, EDVR says so in the log and
carries on without it. This is the same procedure the installer follows, and
what it will tell you it did.

EDHM's uninstaller runs `del d3d11.dll`, which after this is EDVR's file. To
undo the pair cleanly, delete `d3d11.dll` and `edvr.ini`, rename
`d3d11_edhm.dll` back, then run EDHM's uninstaller if you want to.
