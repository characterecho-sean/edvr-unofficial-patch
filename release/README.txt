EDVR native OpenXR release
===========================

EDVR is a graphics and VR compatibility patch for Elite Dangerous: Odyssey.
This release uses the native OpenXR route on Windows. The bundled Khronos
loader selects the system OpenXR runtime; SteamVR is allowed when selected in
Windows, but the SteamVR application is not required by this package.

INSTALLER
---------

Close Elite Dangerous, run edvr-installer.exe, select the game directory and
install. The installer validates the native pair before writing, preserves the
game's original OpenVR DLL for recovery, and creates a local OpenXR config.

MANUAL INSTALL
--------------

1. Close Elite Dangerous.
2. Copy d3d11.dll beside EliteDangerous64.exe.
   Copy edvr.ini only if you do not already have one; keep existing settings.
3. In Openvr\win64, rename the game's openvr_api.dll to
   openvr_api_orig.dll. Do not overwrite an existing original with an EDVR DLL.
4. Copy openvr\openvr_api.dll, openvr\openxr_loader.dll and
   openvr\OPENXR-LOADER-LICENSE.txt into Openvr\win64.
5. Start the game.

If EDHM or another mod owns d3d11.dll, use the installer to preserve and chain
it automatically. For manual chaining, rename that mod's DLL to d3d11_edhm.dll
and set advanced.real_dll = d3d11_edhm.dll in edvr.ini before placing EDVR.

DLSS
----

When included, nvngx_dlss.dll is an optional anti-aliasing component. The
native OpenXR route does not fall back to another VR backend if it is absent.

UNINSTALL
---------

Use the installer recovery action, or remove EDVR's d3d11.dll and the native
OpenVR files, then rename openvr_api_orig.dll back to openvr_api.dll. Keep the
original OpenVR file until recovery is complete.

LICENSE
-------

See LICENSE.txt and openvr/OPENXR-LOADER-LICENSE.txt.
