@echo off
setlocal enabledelayedexpansion
REM ===========================================================================
REM  EDVR release packaging
REM
REM  Produces dist\edvr-<version>.zip containing only what an end user needs:
REM  the two proxies, the config, the installer, a plain-text readme and the
REM  licences -- NVIDIA's for the DLSS runtime the installer carries inside it.
REM
REM  The zip ships the repository's own edvr.ini rather than a separate release
REM  copy, so there is nothing to drift out of sync with what is documented.
REM
REM  Usage:  package.bat <version> [--no-dlss]
REM
REM  A release carries NVIDIA's DLSS runtime, so a build made without the SDK
REM  (build.bat says so, loudly) is refused here -- unless --no-dlss says that
REM  is the intent, for a build on a machine that cannot have the SDK.
REM ===========================================================================

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "VER=%~1"
if "%VER%"=="" (
    echo [edvr] usage: package.bat ^<version^>   e.g. package.bat 0.2.0
    exit /b 1
)
set "STAGE=%ROOT%\dist\edvr-%VER%"
set "ZIP=%ROOT%\dist\edvr-%VER%.zip"
set "NO_DLSS="
if /I "%~2"=="--no-dlss" set "NO_DLSS=1"

REM Build, then test, then package. In that order, always.
REM
REM This used to check only that build\d3d11.dll existed. A stale binary from an
REM earlier source revision would be zipped alongside today's ini and README, and
REM smoke.exe -- built by build.bat, and named in its closing message as the way
REM to check a build -- was invoked by nothing at all. It was entirely possible
REM to ship a DLL that would have failed its own test.
REM Gated with `||`, not `if errorlevel 1`.
REM
REM `if errorlevel N` means "exit code >= N", and a process killed by an access
REM violation exits with a negative NTSTATUS -- so it read a CRASH as success.
REM Measured. `%errorlevel%` is no good either: these gates sit inside
REM parenthesised blocks, where it expands once at parse time. `||` keys off the
REM command's own exit code and is immune to both.
echo [edvr] building before packaging
call "%ROOT%\build.bat" || (
    echo [edvr] ERROR: build failed or crashed; nothing packaged
    exit /b 1
)

echo [edvr] running the build checks
"%ROOT%\build\smoke.exe" "%ROOT%\build\d3d11.dll" || (
    echo [edvr] ERROR: smoke test failed or crashed; nothing packaged
    exit /b 1
)

if not exist "%ROOT%\build\d3d11.dll" (
    echo [edvr] ERROR: build\d3d11.dll not found after building.
    exit /b 1
)

REM NVIDIA's DLSS runtime. build.bat copies it into build\ only when the pinned
REM SDK was found and verified, so its absence here means a build without DLAA
REM whose installer carries no runtime: refused, unless --no-dlss.
if not exist "%ROOT%\build\nvngx_dlss.dll" (
    if not defined NO_DLSS (
        echo [edvr] ERROR: build\nvngx_dlss.dll is missing -- this build has no DLAA and its
        echo        installer carries no runtime, which is not a release. Run
        echo        python tools\fetch_ngx.py and build again, or pass --no-dlss to package
        echo        it anyway, on purpose.
        exit /b 1
    )
    echo [edvr] NOTE: packaging WITHOUT NVIDIA's DLSS runtime, as asked.
)

if exist "%STAGE%" rmdir /s /q "%STAGE%"
if exist "%ZIP%" del /f /q "%ZIP%"
mkdir "%STAGE%" 2>nul

copy /Y "%ROOT%\build\d3d11.dll"    "%STAGE%\d3d11.dll"   >nul
copy /Y "%ROOT%\edvr.ini"           "%STAGE%\edvr.ini"    >nul
copy /Y "%ROOT%\release\README.txt" "%STAGE%\README.txt"  >nul
copy /Y "%ROOT%\LICENSE"            "%STAGE%\LICENSE.txt" >nul
REM NVIDIA's licence rides with the runtime the installer carries: the SDK's
REM terms ask that the runtime be distributed under terms at least as
REM protective, and the notice in the zip is the ordinary way to meet that.
if exist "%ROOT%\build\NVIDIA-DLSS-LICENSE.txt" (
    copy /Y "%ROOT%\build\NVIDIA-DLSS-LICENSE.txt" "%STAGE%\NVIDIA-DLSS-LICENSE.txt" >nul
)

REM The installer, inside the zip as well as beside it. It carries these same
REM files as resources, so the copy in the zip is redundant by design: whoever
REM opens the archive can run the installer OR follow the manual steps, and
REM neither path needs the other.
if exist "%ROOT%\build\edvr-installer.exe" (
    copy /Y "%ROOT%\build\edvr-installer.exe" "%STAGE%\edvr-installer.exe" >nul
) else (
    echo [edvr] NOTE: no build\edvr-installer.exe -- packaging without the installer.
)

REM Both files, always. openvr_api.dll used to be optional here -- shipped when
REM it happened to be built, skipped with a note when it was not -- and that
REM note was the only thing standing between a partial build and a release. A
REM zip without it installs a patch whose transition flash fix can detect and
REM log but never withhold a frame, and whose Explorer Cam does nothing at all,
REM with no way for the person running it to tell.
REM
REM It still goes in its own folder with its own instructions rather than loose
REM in the root, because it REPLACES a file the game owns rather than adding
REM one, and loose next to d3d11.dll it would be copied in by reflex.
if not exist "%ROOT%\build\openvr_api.dll" (
    echo [edvr] ERROR: build\openvr_api.dll is missing, and it is not optional.
    echo        It can only be built where the game's own openvr_api.dll is
    echo        available to generate an export table from: run build.bat on a
    echo        machine with Elite installed, or pass --openvr ^<path^>.
    exit /b 1
)
mkdir "%STAGE%\openvr" 2>nul
copy /Y "%ROOT%\build\openvr_api.dll" "%STAGE%\openvr\openvr_api.dll" >nul
copy /Y "%ROOT%\release\OPENVR.txt"   "%STAGE%\openvr\READ-ME-FIRST.txt" >nul

echo [edvr] staged:
dir /b /s "%STAGE%"

powershell -NoProfile -Command ^
  "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%' -Force"
if errorlevel 1 ( echo [edvr] ERROR: zip failed & exit /b 1 )


REM And on its own, which is the download most people should be given: one
REM executable, nothing to extract, nothing to put in the right folder.
if exist "%ROOT%\build\edvr-installer.exe" (
    copy /Y "%ROOT%\build\edvr-installer.exe" "%ROOT%\dist\edvr-installer-%VER%.exe" >nul
)

echo.
echo [edvr] wrote %ZIP%
for %%F in ("%ZIP%") do echo        %%~zF bytes
powershell -NoProfile -Command ^
  "'       SHA-256 ' + (Get-FileHash '%ZIP%' -Algorithm SHA256).Hash"

if exist "%ROOT%\dist\edvr-installer-%VER%.exe" (
    echo.
    echo [edvr] wrote %ROOT%\dist\edvr-installer-%VER%.exe
    for %%F in ("%ROOT%\dist\edvr-installer-%VER%.exe") do echo        %%~zF bytes
    powershell -NoProfile -Command ^
      "'       SHA-256 ' + (Get-FileHash '%ROOT%\dist\edvr-installer-%VER%.exe' -Algorithm SHA256).Hash"
)
echo.
echo [edvr] Before publishing, check the archive holds only the files listed
echo        above and no logs, shader dumps or game binaries. In particular it
echo        must not contain the game's own openvr_api.dll -- only ours.
echo        Publish BOTH: the .exe for people who want one download, and the
echo        .zip for people who would rather see the files. Say the SHA-256 of
echo        the exe in the release notes -- it is unsigned, so a hash somebody
echo        can check is the only provenance on offer.
exit /b 0
