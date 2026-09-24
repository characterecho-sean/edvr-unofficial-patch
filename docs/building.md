# Building EDVR

How to build EDVR from source, check the build without the game or a headset,
and what the optional DLSS mode and the release package need.

Building needs Visual Studio 2022 with the C++ workload, and Python. First
fetch the pinned official Khronos loader and its notice:

```
python tools\fetch_openxr_loader.py
```

Then build:

```
build.bat
```

That produces the native graphics/runtime pair, `build\openxr_loader.dll`, and
`build\smoke.exe`; the last of these checks the build without the game or a
headset:

```
build\smoke.exe build\d3d11.dll
```

The optional DLSS mode, and an installer that carries NVIDIA's runtime, need
the DLSS SDK on the machine. Its licence keeps it out of this repository, and
it is not in the graphics driver either. This command

```
python tools\fetch_ngx.py
```

fetches one pinned commit of NVIDIA's public SDK repository into
`%LOCALAPPDATA%\EDVR\ngx-sdk` and checks the runtime's hash against the pin in
the script. `EDVR_NGX_SDK` points the build at a copy somewhere else.

Generating the installer's resources needs the native pair and the bundled
loader; there is no legacy OpenVR-only package. `package.bat <version>
--no-dlss` packages a build made without the DLSS SDK. When the build contains
DLSS, the archive keeps its matching DLL and NVIDIA license notice next to the
installer that embeds it.
